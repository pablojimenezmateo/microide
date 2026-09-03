#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/TabReorder.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "Welcome" : "Untitled";
}

}  // namespace

TabCoordinator::TabCoordinator(ProjectCatalogState& project_catalog,
                               ProjectWorkspaceState& current_project_state,
                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      operations_(std::move(operations)) {}


std::string TabCoordinator::ActiveTitle() const {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return EditorTabLabel(state_.focused_group().welcome_surface.viewport);
  }
  return state_.focused_group().open_tabs[state_.focused_group().active_tab_index].title;
}

bool TabCoordinator::Save(std::size_t index) {
  return SaveGroupTab(state_.clamped_focused_group_index(), index);
}

bool TabCoordinator::SaveGroupTab(std::size_t group_index, std::size_t index) {
  if (group_index >= state_.editor_groups.size()) {
    return false;
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index >= group.open_tabs.size()) {
    return false;
  }

  const auto refresh_directory_tree = [this]() {
    state_.directory_tree.Refresh();
    if (operations_.request_automatic_git_sidebar_refresh) {
      operations_.request_automatic_git_sidebar_refresh();
    }
  };

  if (group.open_tabs[index].kind == TabEntry::Kind::Compare &&
      group.open_tabs[index].compare.has_value()) {
    auto& compare_tab = group.open_tabs[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    if (compare_tab.right_viewport.DetectDiskConflict() !=
        editor::TextViewport::DiskConflict::None) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            compare_tab.right_viewport.path().lexically_normal());
      }
      return false;
    }
    if (!compare_tab.right_viewport.Save()) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(compare_tab.right_viewport.path());
      }
      return false;
    }
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(compare_tab.right_viewport.path());
    return true;
  }

  if (group.open_tabs[index].kind == TabEntry::Kind::Merge &&
      group.open_tabs[index].merge.has_value()) {
    auto& merge_tab = group.open_tabs[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (merge_tab.result_viewport.DetectDiskConflict() !=
        editor::TextViewport::DiskConflict::None) {
      if (operations_.request_external_change_banner) {
        operations_.request_external_change_banner(
            merge_tab.result_viewport.path().lexically_normal());
      }
      return false;
    }
    if (!merge_tab.result_viewport.Save()) {
      if (operations_.notify_save_failed) {
        operations_.notify_save_failed(merge_tab.result_viewport.path());
      }
      return false;
    }
    merge_tab.persisted_output_baseline =
        util::SerializeLinesStreaming(editor::LineSpan(merge_tab.result_viewport.lines()),
                                      merge_tab.result_line_ending);
    // Our own atomic write+rename bumps the mtime; sync disk_result_tick to it and
    // clear any prior external-stale flag so the async file-change event does not
    // misread this save as an external modification. Without this, saving the merge
    // result would permanently lock out "Mark Resolved" for the tab.
    merge_tab.disk_result_tick = util::FileModificationTick(merge_tab.result_viewport.path());
    merge_tab.external_result_stale = false;
    refresh_directory_tree();
    operations_.notify_plugin_buffer_save(merge_tab.result_viewport.path());
    return true;
  }

  if (group.open_tabs[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = group.open_tabs[index].editor_state;
  if (!editor_state.has_value()) {
    return false;
  }

  editor::TextViewport* candidate = &editor_state->viewport;
  if (candidate->path().empty()) {
    // Untitled buffers cannot be saved through this path; refuse if dirty.
    return !candidate->dirty();
  }
  const std::filesystem::path normalized_path = candidate->path().lexically_normal();
  if (!candidate->dirty()) {
    operations_.notify_plugin_buffer_save(normalized_path);
    return true;
  }
  // Refuse to overwrite a file that changed on disk since we loaded/last saved
  // it. Surface the external-change banner instead so the user can choose to
  // Reload, Overwrite, or Keep. Checked before formatters run so we don't
  // mutate the buffer for a save we're about to abort.
  if (candidate->DetectDiskConflict() != editor::TextViewport::DiskConflict::None) {
    if (operations_.request_external_change_banner) {
      operations_.request_external_change_banner(normalized_path);
    }
    return false;
  }
  if (operations_.prepare_editor_view_for_save &&
      !operations_.prepare_editor_view_for_save(candidate->path(), *candidate, nullptr)) {
    return false;
  }
  if (!candidate->Save()) {
    if (operations_.notify_save_failed) {
      operations_.notify_save_failed(candidate->path());
    }
    return false;
  }
  operations_.invalidate_editor_blame_path(normalized_path);
  operations_.notify_plugin_buffer_save(normalized_path);
  refresh_directory_tree();
  return true;
}

bool TabCoordinator::ActiveTabIsEditor() const {
  const EditorGroup& group = state_.focused_group();
  return group.has_active_tab() && group.active_tab().kind == TabEntry::Kind::Editor &&
         group.active_tab().editor_state.has_value();
}

TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.focused_group().open_tabs[state_.focused_group().active_tab_index].editor_state.value();
}

const TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &state_.focused_group().open_tabs[state_.focused_group().active_tab_index].editor_state.value();
}

editor::TextViewport* TabCoordinator::ActiveEditorViewport() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return &state_.focused_group().welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

const editor::TextViewport* TabCoordinator::ActiveEditorViewport() const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return &state_.focused_group().welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

void TabCoordinator::Activate(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::Activate");
  perf_label.Field("index", static_cast<long long>(index));
  if (!state_.focused_group().open_tabs[index].path.empty()) {
    perf_label.Field("path", state_.focused_group().open_tabs[index].path);
  }
  util::StartupTrace::Scope trace_scope("TabCoordinator::Activate");
  util::PerformanceTrace::Scope perf_scope(perf_label.View());

  if (state_.focused_group().active_tab_index == index) {
    auto& active_tab = state_.focused_group().open_tabs[index];
    // Re-activating the already-active tab normally needs no load, but a tab left
    // deferred (editor_state unset yet a deferred handle present) must still be
    // hydrated here or its pane renders empty. Guarded on the un-hydrated state so
    // the common re-click stays a no-op and never re-snaps scroll onto the caret.
    if (active_tab.kind == TabEntry::Kind::Editor && !active_tab.editor_state.has_value() &&
        active_tab.deferred_handle.has_value()) {
      (void)LoadEditorTabForActivation(active_tab);
    }
    SyncActiveEditorTabMetadata();
    state_.surface.focus = FocusTarget::Editor;
    operations_.reset_caret_blink();
    const editor::TextViewport* active_vp = ActiveEditorViewport();
    operations_.request_active_tab_redraw(active_tab.kind == TabEntry::Kind::Editor &&
                                          active_vp != nullptr && !active_vp->path().empty());
    return;
  }

  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() && state_.focused_group().active_tab_index != index) {
    SyncActiveEditorTab();
  }

  state_.focused_group().active_tab_index = index;
  auto& tab = state_.focused_group().open_tabs[index];
  // Editor loading is best-effort: if the file disappeared while the IDE was
  // closed, we still want the tab strip + project tree to reflect this tab as
  // the active one (otherwise the activation looks like a no-op to the user).
  // LoadEditorTabForActivation returns false on failure but the post-activation
  // sync below still runs.
  (void)LoadEditorTabForActivation(tab);
  SyncActiveEditorTabMetadata();
  const editor::TextViewport* active_vp =
      (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value())
          ? &tab.editor_state->viewport
          : nullptr;
  const std::filesystem::path active_vp_path =
      active_vp != nullptr ? active_vp->path().lexically_normal() : std::filesystem::path{};
  if (tab.kind == TabEntry::Kind::Compare) {
    operations_.reveal_active_compare_selection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    operations_.reveal_active_merge_selection();
  } else if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty()) {
    util::StartupTrace::Scope select_path_scope("TabCoordinator::Activate::SelectDirectoryPath");
    if (state_.directory_tree.SelectPathIfVisible(active_vp_path)) {
      operations_.reveal_selected_tree_sidebar_line();
    }
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  // Engage the language server for the now-active editor document. This covers the
  // session-restore case (a file already open at startup, activated without ever
  // going through OpenFileInNewTab) and plain tab switches — without it, a
  // restored file's LSP stayed at "Starting..." with no diagnostics/semantic
  // colors until an edit or go-to-definition. SCHEDULED, not run inline: the
  // hydration (didOpen + semantic tokens + inlay hints, which serializes the whole
  // buffer) runs after the tab-switch frame is presented, so switching to a large
  // file never blocks the tab becoming visible. Idempotent, so switches are cheap.
  if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty() &&
      operations_.schedule_lsp_buffer_open) {
    operations_.schedule_lsp_buffer_open(active_vp_path);
  }
  operations_.request_active_tab_redraw(tab.kind == TabEntry::Kind::Editor &&
                                        !active_vp_path.empty());
}

void TabCoordinator::SyncActiveEditorTab() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  auto& editor_state = *tab.editor_state;
  if (editor_state.needs_restore) {
    tab.path = operations_.editor_view_path(editor_state);
    tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
    return;
  }
  editor_state.restored_path = editor_state.viewport.path().lexically_normal();
  editor_state.restored_cursor_line = editor_state.viewport.cursor_line();
  editor_state.restored_cursor_column = editor_state.viewport.cursor_column();
  editor_state.restored_scroll_line = editor_state.viewport.scroll_line();
  editor_state.restored_horizontal_scroll = editor_state.viewport.horizontal_scroll();
  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
      &tab == &state_.focused_group().open_tabs[state_.focused_group().active_tab_index]) {
    SyncActiveEditorTabMetadata();
  }
}

bool TabCoordinator::ActivateCurrentTabAfterStateLoad() {
  // Eagerly hydrate the active editor tab of every non-focused group so a restored
  // split shows content in both panes, not just the focused one. The focused group
  // is hydrated below via Activate().
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    if (gi == state_.focused_group_index) {
      continue;
    }
    EditorGroup& group = state_.editor_groups[gi];
    if (group.active_tab_index >= group.open_tabs.size()) {
      continue;
    }
    TabEntry& tab = group.open_tabs[group.active_tab_index];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      // Skip the preference re-apply for a freshly-hydrated tab: RestoreEditorTab
      // already applied current preferences, and re-running them here would fire
      // EnsureCursorVisible and clobber the restored scroll.
      const bool was_deferred = tab.editor_state->needs_restore;
      if (EnsureEditorTabLoaded(tab) && !was_deferred) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (state_.focused_group().open_tabs.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(state_.focused_group().active_tab_index, state_.focused_group().open_tabs.size() - 1);
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size();
  Activate(active_index);
  return state_.focused_group().active_tab_index == active_index;
}

void TabCoordinator::SyncActiveEditorTabMetadata() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  std::filesystem::path active_path;
  if (tab.editor_state.has_value()) {
    const editor::TextViewport& viewport = tab.editor_state->viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport);
  } else if (tab.deferred_handle.has_value() || !tab.path.empty()) {
    // No live viewport, but the tab has a real identity: a deferred tab whose
    // file failed to open (e.g. deleted out from under the IDE between restore
    // and first activation). Do NOT fall through to the group's welcome surface
    // -- its empty path would clobber the tab's filename/title, stranding the
    // tab as "Welcome"/"" in the strip and breaking the OpenFileInNewTab dedup
    // (keyed on tab.path). Keep the restored identity; recover it from the
    // deferred handle if the path was somehow lost.
    if (tab.path.empty() && tab.deferred_handle.has_value()) {
      tab.path = tab.deferred_handle->path.lexically_normal();
      tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
    }
    active_path = tab.path.lexically_normal();
  } else {
    // Truly empty editor tab (no state, no handle, no path): mirror the group
    // welcome surface, as before.
    const editor::TextViewport& viewport = state_.focused_group().welcome_surface.viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport);
  }
  if (!active_path.empty() && state_.directory_tree.SelectPathIfVisible(active_path)) {
    operations_.reveal_selected_tree_sidebar_line();
  } else if (!active_path.empty() &&
             !state_.directory_tree.HasManuallyCollapsedAncestor(active_path) &&
             state_.directory_tree.SelectPath(active_path)) {
      operations_.reveal_selected_tree_sidebar_line();
  }
}

void TabCoordinator::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  ReloadEditorTabsForPath(path, /*clean_only=*/true);
}

void TabCoordinator::ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path) {
  ReloadEditorTabsForPath(path, /*clean_only=*/false);
}

void TabCoordinator::ReloadEditorTabsForPath(const std::filesystem::path& path, bool clean_only) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  operations_.invalidate_editor_blame_path(normalized_path);

  // Reload every editor view on this path across ALL groups — a split view of the same
  // file in the non-focused group must not be left showing stale content (nor keep a
  // stale disk_signature that would later misfire self-write echo suppression).
  // Compare, do not materialize: this runs over every tab in every group, twice
  // (the any-match probe and the apply loop), and the dominant caller is opening a
  // file that is already open. Building a normalized `path` value per tab per scan
  // made that quadratic in tab count at ~12 allocations a step
  // (TD-2026-08-06-159).
  const auto matches = [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
           EditorViewPathIs(*tab.editor_state, normalized_path);
  };
  bool any_match = false;
  for (const EditorGroup& group : state_.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      if (matches(tab) && !(clean_only && TabStateIsDirty(tab))) {
        any_match = true;
        break;
      }
    }
    if (any_match) {
      break;
    }
  }
  if (!any_match) {
    return;
  }

  // Nothing changed underneath us: every open view of this path already records
  // the signature the file has right now, so a reload would read the whole file
  // off disk only to produce byte-identical content -- and then throw away every
  // derived cache in the process (widths, highlights, folds, undo history).
  //
  // The dominant caller is opening a file that is already open, which is what
  // Ctrl+P to a file you are already looking at does. VSCode just focuses the
  // tab there; before this, a 50k-line file paid a full file read plus a
  // whole-document width rebuild on every such open (TD-2026-08-06-138). The
  // focus-regain sweep (ReloadCleanOpenBuffersFromDisk) paid it once per open
  // buffer.
  //
  // `clean_only` only: the from-disk form is the banner's explicit "discard my
  // edits and reload", and a dirty buffer's recorded signature can legitimately
  // still match the file it was loaded from -- skipping there would silently
  // refuse to discard. mtime+size is the same equality the self-write echo
  // suppression in WorkspaceShellProjectChanges already trusts.
  if (clean_only &&
      DiskSignatureMatchesOpenView(normalized_path, util::StatFileSignature(normalized_path))) {
    return;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(normalized_path)) {
    return;
  }
  operations_.apply_editor_preferences(reopened_view);
  operations_.apply_detected_indent_on_open(reopened_view);

  // Capture just the delta the LSP re-sync needs (pre-reload line count + the first
  // line whose content differs from the reloaded buffer) from the first view we
  // replace. Both the old view and `reopened_view` (the after-content) exist here,
  // so this compares them line-by-line without materializing either whole buffer —
  // the full didChange later streams straight from the reloaded viewport. All
  // matching views share this path's single server document, so one sync suffices.
  std::size_t lsp_before_line_count = 0;
  std::size_t lsp_first_changed_line = 0;
  bool captured_lsp_before = false;

  const std::size_t focused_index = state_.clamped_focused_group_index();
  for (std::size_t g = 0; g < state_.editor_groups.size(); ++g) {
    EditorGroup& group = state_.editor_groups[g];
    const bool is_focused_group = g == focused_index;
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      TabEntry& tab = group.open_tabs[i];
      if (!matches(tab) || (clean_only && TabStateIsDirty(tab))) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      if (!captured_lsp_before) {
        const editor::TextBuffer& old_lines = editor_state.viewport.lines();
        const editor::TextBuffer& new_lines = reopened_view.lines();
        lsp_before_line_count = old_lines.size();
        const std::size_t common = std::min(old_lines.size(), new_lines.size());
        while (lsp_first_changed_line < common &&
               old_lines.LineView(lsp_first_changed_line) ==
                   new_lines.LineView(lsp_first_changed_line)) {
          ++lsp_first_changed_line;
        }
        captured_lsp_before = true;
      }
      const editor::TextViewport* current_view = &editor_state.viewport;
      editor::TextViewport restored_view = reopened_view;
      restored_view.SetViewportSize(current_view->visible_lines(), current_view->visible_columns());
      restored_view.ApplyRestoredViewState(current_view->cursor_line(), current_view->cursor_column(),
                                           current_view->scroll_line(),
                                           current_view->horizontal_scroll());
      // Moved, not copied: `restored_view` is dead after this and its layout
      // cache now survives a move, so copying would deep-copy a per-line width
      // table (one machine word per line of the document) to throw it away one
      // statement later. Read the restored fields back off the tab's own
      // viewport, which is where they live now.
      editor_state.viewport = std::move(restored_view);
      editor_state.restored_path = normalized_path;
      editor_state.restored_cursor_line = editor_state.viewport.cursor_line();
      editor_state.restored_cursor_column = editor_state.viewport.cursor_column();
      editor_state.restored_scroll_line = editor_state.viewport.scroll_line();
      editor_state.restored_horizontal_scroll = editor_state.viewport.horizontal_scroll();
      editor_state.needs_restore = false;
      editor_state.folding_model->Clear();
      if (is_focused_group && i == group.active_tab_index) {
        SyncActiveEditorTabMetadata();
        operations_.request_editor_surface_redraw();
      }
    }
  }

  // Re-sync the LSP server's document mirror to the reloaded content (full
  // didChange). Fired once after all views are swapped, since the server tracks a
  // single document per path regardless of how many split views show it.
  if (captured_lsp_before && operations_.notify_lsp_buffer_reloaded) {
    operations_.notify_lsp_buffer_reloaded(reopened_view, lsp_before_line_count,
                                           lsp_first_changed_line);
  }
}

bool TabCoordinator::OpenUntitled() {
  if (state_.root.empty()) {
    return false;
  }
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Refuse a `tab`-command flood; see kMaxOpenTabsPerGroup.
  }

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  operations_.apply_editor_preferences(untitled_view);

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
      .editor_state = operations_.make_editor_tab_state(untitled_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  return true;
}
bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::OpenFileInNewTab");
  perf_label.Field("path", path);
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  if (state_.root.empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });

  {
    util::PerformanceTrace::Scope select_path_scope(
        "TabCoordinator::OpenFileInNewTab::SelectDirectoryPath");
    if (!state_.directory_tree.SelectPathIfVisible(normalized_path)) {
      state_.directory_tree.SelectPath(normalized_path);
    }
  }

  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t existing_index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(existing_index)) {
      ReloadCleanEditorTabsForPath(normalized_path);
    }
    Activate(existing_index);
    return true;
  }

  // Enforce the per-group ceiling before touching disk: an already-open file
  // reuses its tab via the dedup above, so a brand-new open past the cap is
  // refused here — before the file read + indent detection below — so a flood of
  // distinct paths cannot pay that I/O only to be rejected.
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }

  editor::TextViewport opened_view;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::OpenFileInNewTab::OpenFile");
    if (!opened_view.OpenFile(normalized_path)) {
      return false;
    }
  }
  operations_.apply_editor_preferences(opened_view);
  operations_.apply_detected_indent_on_open(opened_view);

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.notify_plugin_buffer_open(normalized_path);
  operations_.request_active_tab_redraw(true);
  return true;
}
bool TabCoordinator::OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                                 std::string_view content,
                                                 std::string_view title) {
  if (state_.root.empty() || virtual_path.empty()) {
    return false;
  }

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == virtual_path;
                               });
  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(index) && existing->editor_state.has_value() &&
        operations_.editor_view_path(*existing->editor_state) == virtual_path) {
      existing->editor_state->viewport.ReloadPreservingViewState(content);
    }
    Activate(index);
    return true;
  }

  editor::TextViewport viewport;
  viewport.LoadContent(content, virtual_path);
  operations_.apply_editor_preferences(viewport);
  operations_.apply_detected_indent_on_open(viewport);

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // At the per-group ceiling; see kMaxOpenTabsPerGroup.
  }
  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = virtual_path,
      .title = std::string(title),
      .editor_state = operations_.make_editor_tab_state(viewport),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  return true;
}
void TabCoordinator::ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                               std::string_view content) {
  if (virtual_path.empty()) {
    return;
  }

  // Walk EVERY editor group, not just the focused one: a virtual document open in
  // both split panes must refresh both copies, matching ReloadEditorTabsForPath.
  bool reloaded_any = false;
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    EditorGroup& group = state_.editor_groups[gi];
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      auto& tab = group.open_tabs[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          TabStateIsDirty(tab)) {
        continue;
      }
      if (operations_.editor_view_path(*tab.editor_state) != virtual_path) {
        continue;
      }
      tab.editor_state->viewport.ReloadPreservingViewState(content);
      reloaded_any = true;
      if (i == group.active_tab_index) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (reloaded_any) {
    operations_.request_active_tab_redraw(false);
  }
}
std::filesystem::path TabCoordinator::LspCloseCandidatePath(const TabEntry& tab) const {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
    return operations_.editor_view_path(*tab.editor_state);
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    const auto& compare_tab = *tab.compare;
    if (compare_tab.right_editable && !compare_tab.right_viewport.path().empty()) {
      return compare_tab.right_viewport.path().lexically_normal();
    }
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    const auto& merge_tab = *tab.merge;
    if (!merge_tab.result_viewport.path().empty()) {
      return merge_tab.result_viewport.path().lexically_normal();
    }
  }
  return {};
}

void TabCoordinator::MaybeNotifyLspClose(const TabEntry& tab) {
  const std::filesystem::path path = LspCloseCandidatePath(tab);
  if (!path.empty() && operations_.count_open_buffer_views(path) == 1) {
    operations_.notify_lsp_buffer_close(path);
  }
}

void TabCoordinator::Close(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  const bool closing_active = index == state_.focused_group().active_tab_index;
  const TabEntry& closing_tab = state_.focused_group().open_tabs[index];

  // No SyncActiveEditorTab() here: closing a non-active tab does not change the
  // active tab, and every persistence/deactivation path re-syncs the active tab
  // (SaveSessionState and StoreCurrentProjectState both call it), so capturing its
  // viewport now would be redundant work — costly in bulk closes via the tree
  // traversals in SyncActiveEditorTabMetadata.
  MaybeNotifyLspClose(closing_tab);

  state_.focused_group().open_tabs.erase(state_.focused_group().open_tabs.begin() + static_cast<std::ptrdiff_t>(index));

  if (state_.focused_group().open_tabs.empty()) {
    // Closing the last tab of a split group collapses that group; the surviving
    // group takes the full editor area.
    if (state_.editor_groups.size() >= 2) {
      CollapseFocusedGroup();
      const editor::TextViewport* active_vp = ActiveEditorViewport();
      operations_.ensure_active_tab_visible();
      operations_.request_active_tab_redraw(active_vp != nullptr && !active_vp->path().empty());
      return;
    }
    state_.focused_group().active_tab_index = 0;
    state_.focused_group().tab_scroll_index = 0;
    state_.focused_group().welcome_surface.viewport.SetPlaceholderText("microide\n\n"
                                            "Project loaded.\n"
                                            "Use the sidebar to open files.\n");
    state_.surface.focus = FocusTarget::Editor;
    operations_.request_active_tab_redraw(false);
    return;
  }

  if (index < state_.focused_group().active_tab_index) {
    --state_.focused_group().active_tab_index;
  } else if (index == state_.focused_group().active_tab_index) {
    state_.focused_group().active_tab_index = std::min(index, state_.focused_group().open_tabs.size() - 1);
    auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
    if (tab.kind == TabEntry::Kind::Editor) {
      // Promote the neighbor through the same loader Activate uses so a deferred
      // (session-restored, never-activated) tab restores its cursor/scroll/
      // selection instead of opening fresh at (0,0).
      (void)LoadEditorTabForActivation(tab);
    } else if (tab.kind == TabEntry::Kind::Compare) {
      // Promoting a compare/merge tab on close must scroll its active selection
      // into view, exactly as Activate() does — otherwise the revealed tab's
      // selected hunk can sit off-screen until the next interaction.
      operations_.reveal_active_compare_selection();
    } else if (tab.kind == TabEntry::Kind::Merge) {
      operations_.reveal_active_merge_selection();
    }
    if (!tab.path.empty()) {
      state_.directory_tree.SelectPath(tab.path);
      operations_.reveal_selected_tree_sidebar_line();
    }
    state_.surface.focus = FocusTarget::Editor;
  }

  state_.focused_group().tab_scroll_index = std::clamp(state_.focused_group().tab_scroll_index, 0,
                                       std::max(0, static_cast<int>(state_.focused_group().open_tabs.size()) - 1));
  operations_.ensure_active_tab_visible();
  if (closing_active) {
    const editor::TextViewport* active_vp = ActiveEditorViewport();
    operations_.request_active_tab_redraw(active_vp != nullptr && !active_vp->path().empty());
  } else {
    operations_.request_tab_strip_redraw();
  }
}
bool TabCoordinator::MoveActiveTo(std::size_t index) {
  if (!ReorderActive(state_.focused_group().open_tabs, state_.focused_group().active_tab_index, index)) {
    return false;
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_tab_strip_redraw();
  return true;
}

}  // namespace microide::workspace
