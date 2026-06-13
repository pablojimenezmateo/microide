#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "project/SubprocessHelper.h"
#include "util/StringUtil.h"
#include "workspace/EditorTabService.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceTabCoordinator.h"

namespace microide::workspace {

namespace {

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines(), viewport.line_ending());
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
          .notify_lsp_buffer_close =
              [this](const std::filesystem::path& path) { NotifyLspBufferClose(path); },
          .count_open_buffer_views =
              [this](const std::filesystem::path& path) { return CountOpenBufferViews(path); },
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
              [this](const TabEntry::EditorTabState::EditorViewState& view) {
                return EditorViewPath(view);
              },
          .find_editor_view =
              [this](TabEntry::EditorTabState& editor_tab, std::size_t leaf_id) {
                return FindEditorView(editor_tab, leaf_id);
              },
          .normalize_editor_split_tree =
              [this](TabEntry::EditorTabState& editor_tab) { NormalizeEditorSplitTree(editor_tab); },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_tab_redraw =
              [this](bool include_tree_sidebar) { RequestActiveTabRedraw(include_tree_sidebar); },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
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

bool WorkspaceShell::PrepareEditorViewportForSave(const std::filesystem::path& path,
                                                  editor::TextViewport& viewport,
                                                  std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (path.empty()) {
    return true;
  }

  std::string text = SerializeViewportText(viewport);
  if (plugin_runtime_.enabled() && !save_participant_registry_.Specs().empty() &&
      !plugin_runtime_.Host().RunSaveParticipants(path, &text, error_message)) {
    return false;
  }

  const std::string filetype = editor::runtime_syntax::DetectFiletype(path, viewport.lines());
  if (const FormatterSpec* formatter =
          filetype.empty() ? nullptr : formatter_registry_.FindFormatter(filetype);
      formatter != nullptr && !formatter->command.empty()) {
    // Save is synchronous from the UI's perspective, so the formatter has to complete before
    // we return. Running it inline avoids the misleading executor-post-then-wait pattern that
    // implied background work but still blocked the calling thread.
    platform::SubprocessResult result =
        project::RunSubprocess(formatter->command,
                               platform::SubprocessOptions{
                                   .cwd = context_.current_project_state.root,
                                   .stdin_text = text,
                                   .environment_overrides = {},
                               });

    if (!result.success()) {
      if (error_message != nullptr) {
        *error_message = "formatter '" + formatter->id + "' failed";
        if (!result.stderr_text.empty()) {
          *error_message += ": " + result.stderr_text;
        }
      }
      return true;
    }
    if (!result.stdout_text.empty()) {
      text = result.stdout_text;
    }
  }

  if (text == SerializeViewportText(viewport)) {
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
  if (index >= context_.current_project_state.open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = context_.current_project_state.open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(context_.current_project_state.root, path, tab.title, TabIsDirty(index)).display_title;
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t index) const {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return {};
  }

  const TabEntry& tab = context_.current_project_state.open_tabs[index];
  std::filesystem::path path = tab.path;
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    path = tab.compare->path;
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    path = tab.merge->output_path;
  }
  return BuildWorkspaceTabTextModel(context_.current_project_state.root, path, tab.title, TabIsDirty(index)).tooltip_label;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndices();
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.open_tabs.size());
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    if (TabCoordinator::TabStateIsDirty(state.open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndicesForProject(project_index);
}

void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  MakeEditorTabService().ReloadCleanEditorTabsForPath(path);
}

bool WorkspaceShell::OpenUntitledTab() {
  return MakeEditorTabService().OpenUntitled();
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  return MakeEditorTabService().OpenFileInNewTab(path);
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
    for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
      const auto& tab = context_.current_project_state.open_tabs[i];
      if (tab.kind == TabEntry::Kind::Editor && tab.path == normalized_path) {
        ActivateTab(i);
        viewport = ActiveEditorViewport();
        break;
      }
    }
  }

  if (viewport != nullptr) {
    viewport->MoveCursorTo(line, column);
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  return MakeEditorTabService().ReopenActive();
}

}  // namespace microide::workspace
