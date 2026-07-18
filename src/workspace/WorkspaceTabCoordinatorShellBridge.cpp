#include "workspace/WorkspaceShell.h"

#include "workspace/SettingFlags.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "project/SubprocessHelper.h"
#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/EditorTabService.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceTabCoordinator.h"

namespace microide::workspace {

namespace {

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines().Snapshot(), viewport.line_ending());
}

void RestoreViewportText(editor::TextViewport& viewport, std::string_view text) {
  const std::size_t visible_lines = viewport.visible_lines();
  const std::size_t visible_columns = viewport.visible_columns();
  const std::size_t cursor_line = viewport.cursor_line();
  const std::size_t cursor_column = viewport.cursor_column();
  const std::size_t scroll_line = viewport.scroll_line();
  const std::size_t horizontal_scroll = viewport.horizontal_scroll();
  const auto selection = viewport.selection_range();
  const std::filesystem::path path = viewport.path();
  const auto line_ending = viewport.line_ending();

  viewport.LoadContent(text, path, line_ending);
  viewport.SetViewportSize(visible_lines, visible_columns);
  viewport.MoveCursorTo(cursor_line, cursor_column);
  viewport.SetScrollLine(scroll_line);
  viewport.SetHorizontalScroll(horizontal_scroll);
  if (selection.has_value()) {
    viewport.MoveCursorTo(selection->start.line, selection->start.column);
    viewport.MoveCursorTo(selection->end.line, selection->end.column, true);
  }
}

}  // namespace

TabCoordinator WorkspaceShell::MakeTabCoordinator() {
  return TabCoordinator(
      context_.project_catalog,
      context_.current_project_state,
      TabCoordinator::Operations{
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .notify_plugin_buffer_save =
              [this](const std::filesystem::path& path) { NotifyPluginBufferSave(path); },
          .notify_plugin_buffer_open =
              [this](const std::filesystem::path& path) { NotifyPluginBufferOpen(path); },
          .schedule_lsp_buffer_open =
              [this](const std::filesystem::path& path) {
                // Record the hydration and request a frame; the actual didOpen +
                // token/inlay work runs post-present in OnFramePresented so a large
                // buffer's hydration never blocks the tab switch (TD-2026-07-17A-033).
                lsp_service_.ScheduleBufferOpen(path);
                RequestWindowRedraw();
              },
          .notify_lsp_buffer_close =
              [this](const std::filesystem::path& path) { NotifyLspBufferClose(path); },
          .notify_lsp_buffer_reloaded =
              [this](const editor::TextViewport& viewport,
                     const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                lsp_service_.SyncLspForBufferChange(viewport, before_lines, after_lines);
              },
          .count_open_buffer_views =
              [this](const std::filesystem::path& path) { return CountOpenBufferViews(path); },
          .open_buffer_view_counts = [this]() { return OpenBufferViewCounts(); },
          .prepare_editor_view_for_save =
              [this](const std::filesystem::path& path,
                     editor::TextViewport& viewport,
                     std::string* error_message) {
                return PrepareEditorViewportForSave(path, viewport, error_message);
              },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .apply_detected_indent_on_open =
              [this](editor::TextViewport& viewport) { ApplyDetectedIndentOnOpen(viewport); },
          .make_editor_tab_state =
              [this](const editor::TextViewport& viewport) { return MakeEditorTabState(viewport); },
          .editor_view_path =
              [this](const TabEntry::EditorTabState& editor_state) {
                return EditorViewPath(editor_state);
              },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_tab_redraw =
              [this](bool include_tree_sidebar) { RequestActiveTabRedraw(include_tree_sidebar); },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .invalidate_editor_tab_geometry =
              [this]() { tab_strip_service_.InvalidateEditorTabGeometry(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .request_automatic_git_sidebar_refresh =
              [this]() { RequestAutomaticGitSidebarRefresh(); },
          .activate_tab = [this](std::size_t index) { ActivateTab(index); },
          .request_external_change_banner =
              [this](const std::filesystem::path& path) {
                SetEditorBanner(context_.current_project_state,
                                EditorBannerState::Kind::ExternalChange, path);
                RequestEditorSurfaceRedraw();
              },
          .notify_save_failed =
              [this](const std::filesystem::path& path) {
                const std::string name =
                    path.empty() ? std::string("file") : path.filename().string();
                Notify(NotificationService::Tone::Error, "Failed to save " + name);
              },
      });
}

EditorTabService WorkspaceShell::MakeEditorTabService() {
  return EditorTabService(MakeTabCoordinator());
}

std::string WorkspaceShell::ActiveTabTitle() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().ActiveTitle();
}

bool WorkspaceShell::SaveTab(std::size_t index) {
  std::lock_guard<std::mutex> lock(save_tab_mutex_);
  return MakeEditorTabService().Save(index);
}

bool WorkspaceShell::SaveGroupTab(std::size_t group_index, std::size_t index) {
  std::lock_guard<std::mutex> lock(save_tab_mutex_);
  return MakeEditorTabService().SaveGroupTab(group_index, index);
}

void WorkspaceShell::MaybeAutosaveDirtyTabs(bool on_focus_change) {
  const std::string mode = GetSettingValue("editor.autosave").value_or("off");
  const bool trigger =
      on_focus_change ? (mode == "on_focus_change") : (mode == "after_delay");
  if (!trigger) {
    return;
  }
  // TabCoordinator::Save refuses untitled buffers and surfaces the external-change
  // banner on a disk conflict, so autosave never pops a dialog or clobbers a file
  // changed on disk. Suppress the synchronous format-on-save subprocess for the
  // duration: an autosave (focus change / after delay) must not stall the UI thread
  // on an external formatter — most visibly during window blur / alt-tab.
  autosave_suppress_format_on_save_ = true;
  // Flush every dirty tab across ALL editor groups, not just the focused one, so a
  // buffer dirtied in the non-focused split group is autosaved too (VSCode parity).
  for (const GroupTabRef& ref : MakeEditorTabService().DirtyGroupTabs()) {
    SaveGroupTab(ref.group_index, ref.tab_index);
  }
  autosave_suppress_format_on_save_ = false;
}

Uint64 WorkspaceShell::AutosaveDelayMs() const {
  const int parsed = util::ParseIntOr(GetSettingValue("editor.autosave.delay_ms"), 1000);
  return static_cast<Uint64>(std::clamp(parsed, 200, 60000));
}

void WorkspaceShell::MaybeArmAutosaveTimer() {
  // Only the "after delay" mode uses the debounce; other modes leave the timer idle.
  if (GetSettingValue("editor.autosave").value_or("off") != "after_delay") {
    autosave_armed_ = false;
    autosave_last_viewport_ = nullptr;
    return;
  }
  // Detect a real buffer mutation via the active editable viewport's content revision.
  // A path-less (untitled) or absent buffer has nothing to autosave, so we do not arm.
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  // content_revision() is per-viewport, so switching tabs changes the sampled value
  // without any edit having occurred. Re-baseline against the newly-active viewport
  // WITHOUT resetting the debounce or disarming: a timer armed by an edit on the
  // previous tab must survive the switch so that buffer still autosaves — the flush
  // (MaybeAutosaveDirtyTabs) saves every dirty tab, not just the active one.
  if (viewport != autosave_last_viewport_) {
    autosave_last_viewport_ = viewport;
    autosave_last_content_revision_ = viewport->content_revision();
    return;
  }
  const std::uint64_t revision = viewport->content_revision();
  if (revision == autosave_last_content_revision_) {
    return;  // No edit since the last sample (navigation/focus only): keep the debounce.
  }
  autosave_last_content_revision_ = revision;
  autosave_edit_epoch_ms_ = SDL_GetTicks();
  // Arm while any saveable tab is dirty, not only the active one (and across all
  // editor groups, so a dirty non-focused-group tab keeps the flush armed): an edit
  // that reverts the active buffer to clean must still leave the timer armed to flush
  // another dirty tab.
  autosave_armed_ = !MakeEditorTabService().DirtyGroupTabs().empty();
}

void WorkspaceShell::MaybeArmSessionFlushTimer() {
  // Always-on crash-safety debounce (no editor.autosave gate). Detect a real buffer
  // mutation via the active editable viewport's content revision, mirroring the autosave
  // arm, but WITHOUT the path-backed gate so untitled dirty buffers are covered too.
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return;
  }
  // content_revision() is per-viewport; a tab switch changes the sampled value with no
  // edit. Re-baseline against the newly-active viewport WITHOUT resetting the debounce
  // or disarming, so a flush armed by an edit on another tab still fires (SaveSessionState
  // persists every dirty tab, not just the active one).
  if (viewport != session_flush_last_viewport_) {
    session_flush_last_viewport_ = viewport;
    session_flush_last_content_revision_ = viewport->content_revision();
    return;
  }
  const std::uint64_t revision = viewport->content_revision();
  if (revision == session_flush_last_content_revision_) {
    return;  // Navigation/focus only: keep any pending debounce.
  }
  session_flush_last_content_revision_ = revision;
  session_flush_edit_epoch_ms_ = SDL_GetTicks();
  // Arm only when there is unsaved content somewhere to persist (across all groups).
  session_flush_armed_ = !MakeEditorTabService().DirtyGroupTabs().empty();
}

std::optional<Uint32> WorkspaceShell::NextAutosaveDelayMs() const {
  if (!autosave_armed_) {
    return std::nullopt;
  }
  const Uint64 delay = AutosaveDelayMs();
  const Uint64 elapsed = SDL_GetTicks() - autosave_edit_epoch_ms_;
  if (elapsed >= delay) {
    return static_cast<Uint32>(1);  // Deadline passed: wake immediately to save.
  }
  return static_cast<Uint32>(std::max<Uint64>(1, delay - elapsed));
}

bool WorkspaceShell::PrepareEditorViewportForSave(const std::filesystem::path& path,
                                                  editor::TextViewport& viewport,
                                                  std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (path.empty()) {
    return true;
  }

  // Fast-return BEFORE serializing the whole buffer when no save transform can run:
  // no active plugin save participants AND no enabled formatter for this filetype.
  // The common no-plugin/no-formatter save then pays zero preparation serialization
  // (TextViewport::Save does its own single serialize), instead of the previous
  // two-or-three full-buffer passes. DetectFiletype only scans the first lines, so the
  // filetype probe is cheap. (TD-2026-07-16-16.)
  const bool has_save_participants =
      plugin_runtime_.enabled() && !save_participant_registry_.Specs().empty();
  const std::string filetype = editor::runtime_syntax::DetectFiletype(path, viewport.lines());
  // Autosave suppresses the formatter so a background write never blocks the UI thread
  // on an external subprocess; explicit saves still format.
  const bool format_on_save = !autosave_suppress_format_on_save_ &&
                              SettingFlagEnabled(GetSettingValue("editor.format_on_save"), true);
  const FormatterSpec* formatter =
      (format_on_save && !filetype.empty()) ? FindFormatter(formatter_registry_, filetype)
                                            : nullptr;
  const bool has_formatter = formatter != nullptr && !formatter->command.empty();
  if (!has_save_participants && !has_formatter) {
    return true;  // nothing will transform the text; skip preparation serialization
  }

  std::string text = SerializeViewportText(viewport);
  const std::string original_text = text;  // to detect whether a transform changed it
  if (has_save_participants &&
      !plugin_runtime_.Host().RunSaveParticipants(path, &text, error_message)) {
    return false;
  }

  if (has_formatter) {
    // Save is synchronous from the UI's perspective, so the formatter has to complete before
    // we return. Running it inline avoids the misleading executor-post-then-wait pattern that
    // implied background work but still blocked the calling thread. To keep a hung or
    // pathologically slow formatter from freezing the UI indefinitely, bound the run with a
    // deadline: on expiry the child is killed and the file saves unformatted (warned below).
    // The cap is generous so legitimate slow formatters on large files still complete.
    constexpr int kFormatterTimeoutMs = 5000;
    platform::SubprocessResult result =
        project::RunSubprocess(formatter->command,
                               platform::SubprocessOptions{
                                   .cwd = context_.current_project_state.root,
                                   .stdin_text = text,
                                   .environment_overrides = {},
                                   .timeout_ms = kFormatterTimeoutMs,
                               });

    if (!result.success()) {
      if (error_message != nullptr) {
        *error_message = result.timed_out
                             ? "formatter '" + formatter->id + "' timed out"
                             : "formatter '" + formatter->id + "' failed";
        if (!result.stderr_text.empty()) {
          *error_message += ": " + result.stderr_text;
        }
      }
      // The file still saves (unformatted); warn so the silent formatter failure
      // is visible rather than swallowed.
      Notify(NotificationService::Tone::Warning,
             result.timed_out
                 ? "Formatter '" + formatter->id + "' timed out; saved unformatted"
                 : "Formatter '" + formatter->id + "' failed; saved unformatted");
      return true;
    }
    if (!result.stdout_text.empty()) {
      text = result.stdout_text;
    }
  }

  // Compare against the snapshot captured before transforms, not a fresh re-serialize:
  // when no participant/formatter changed the text there is nothing to apply. (TD-16.)
  if (text == original_text) {
    return true;
  }

  RestoreViewportText(viewport, text);
  viewport.SetDirty(true);
  return true;
}

bool WorkspaceShell::OpenVirtualDocumentInNewTab(std::string_view uri) {
  if (context_.current_project_state.root.empty() || uri.empty()) {
    return false;
  }

  const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(std::string(uri));
  if (document == nullptr) {
    return false;
  }
  return MakeEditorTabService().OpenVirtualDocumentInNewTab(std::filesystem::path(document->uri),
                                                            document->content, document->uri);
}

void WorkspaceShell::ReloadVirtualDocumentTabs(std::string_view uri) {
  if (uri.empty()) {
    return;
  }

  const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(std::string(uri));
  if (document == nullptr) {
    return;
  }
  MakeEditorTabService().ReloadVirtualDocumentTabs(std::filesystem::path(document->uri),
                                                   document->content);
}

bool WorkspaceShell::IsReadOnlyVirtualDocument(const std::filesystem::path& path) const {
  if (path.empty()) {
    return false;
  }
  for (const std::string& uri : virtual_document_registry_.DocumentUris()) {
    if (std::filesystem::path(uri) != path) {
      continue;
    }
    const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(uri);
    return document != nullptr && !document->editable;
  }
  return false;
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().IsDirty(index);
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  return TabDisplayTitle(context_.current_project_state.focused_group_index, index);
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t group_index, std::size_t index) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() ||
      index >= state.editor_groups[group_index].open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = state.editor_groups[group_index].open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(state.root, path, tab.title,
                                    TabCoordinator::TabStateIsDirty(tab)).display_title;
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t index) const {
  return TabTooltipLabel(context_.current_project_state.focused_group_index, index);
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t group_index, std::size_t index) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() ||
      index >= state.editor_groups[group_index].open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = state.editor_groups[group_index].open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(state.root, path, tab.title,
                                    TabCoordinator::TabStateIsDirty(tab)).tooltip_label;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndices();
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    if (TabCoordinator::TabStateIsDirty(state.focused_group().open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndicesForProject(project_index);
}

std::vector<GroupTabRef> WorkspaceShell::DirtyEditorGroupTabs() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyGroupTabs();
}

std::vector<GroupTabRef> WorkspaceShell::DirtyEditorGroupTabsForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)
      ->MakeEditorTabService()
      .DirtyGroupTabsForProject(project_index);
}

void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  MakeEditorTabService().ReloadCleanEditorTabsForPath(path);
}

bool WorkspaceShell::OpenUntitledTab() {
  return MakeEditorTabService().OpenUntitled();
}

bool WorkspaceShell::SplitEditorGroup(EditorSplitOrientation orientation) {
  return MakeEditorTabService().SplitEditorGroup(orientation);
}

bool WorkspaceShell::FocusOtherEditorGroup() {
  return MakeEditorTabService().FocusOtherGroup();
}

void WorkspaceShell::FocusEditorGroup(std::size_t group_index) {
  ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() || group_index == state.focused_group_index) {
    state.surface.focus = FocusTarget::Editor;
    return;
  }
  state.focused_group_index = group_index;
  state.surface.focus = FocusTarget::Editor;
  RequestChromeRedraw();
  RequestEditorSurfaceRedraw();
}

bool WorkspaceShell::CloseEditorGroup() {
  return MakeEditorTabService().CloseEditorGroup();
}

std::size_t WorkspaceShell::EditorGroupCount() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().EditorGroupCount();
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  const bool opened = MakeEditorTabService().OpenFileInNewTab(path);
  if (opened) {
    // Record into the recent-files MRU, scoped to the active project. Resolve to an
    // absolute path so finder/welcome lookups match regardless of the caller's input.
    const std::filesystem::path& root = context_.current_project_state.root;
    if (!root.empty()) {
      const std::filesystem::path absolute = path.is_absolute() ? path : root / path;
      recents_service_.RecordFileOpen(absolute, root);
    }
  }
  return opened;
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  // Reordering the open_tabs vector invalidates the cached display_titles /
  // tooltip_labels / widths in TabStripService, which only key on
  // (tab_count, window_width). Without this drop, the next ComputeVisibleTabs
  // call hits a stale cache and the rendered tab labels stay in the
  // pre-reorder positions even though the underlying tabs have moved — so
  // the tab strip shows the wrong labels while the active editor content
  // already reflects the new order.
  tab_strip_service_.InvalidateEditorTabGeometry();
  return MakeEditorTabService().MoveActiveTo(index);
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().FindIndexBySpecifier(
      specifier, error_message);
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  (void)OpenFileInNewTab(path);
}

void WorkspaceShell::OpenFileAtLocation(const std::filesystem::path& path,
                                        std::size_t line,
                                        std::size_t column) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  OpenFile(path);

  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || viewport->path().lexically_normal() != normalized_path) {
    for (std::size_t i = 0; i < context_.current_project_state.focused_group().open_tabs.size(); ++i) {
      const auto& tab = context_.current_project_state.focused_group().open_tabs[i];
      if (tab.kind == TabEntry::Kind::Editor && tab.path == normalized_path) {
        ActivateTab(i);
        viewport = ActiveEditorViewport();
        break;
      }
    }
  }

  // Only move the caret once we have actually landed on the requested file. If the
  // open failed (per-group tab cap reached, unreadable file, …) the fallback search
  // finds no matching tab and `viewport` still points at the previously-active tab —
  // relocating its caret would scroll/jump the wrong buffer.
  if (viewport != nullptr && viewport->path().lexically_normal() == normalized_path) {
    viewport->MoveCursorTo(line, column);
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  return MakeEditorTabService().ReopenActive();
}

}  // namespace microide::workspace
