#include "workspace/WorkspaceActionServices.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

WorkspaceActionContext::WorkspaceActionContext(ProjectCatalogState& project_catalog,
                                               ProjectWorkspaceState& current_project_state,
                                               float& ui_scale,
                                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      ui_scale_(ui_scale),
      operations_(std::move(operations)) {}

void WorkspaceActionContext::PrepareForAction(ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    operations_.close_tree_context_menu();
  }
}

bool WorkspaceActionContext::RejectAction(ActionSource source, std::string feedback) {
  return operations_.reject_action(source, std::move(feedback));
}

SidebarViewRequest WorkspaceActionContext::ParseSidebarViewRequest(
    const std::vector<std::string>& args) const {
  return operations_.parse_sidebar_view_request(args);
}

bool WorkspaceActionContext::HasProjectRoot() const {
  return !state_.root.empty();
}

bool WorkspaceActionContext::HasActiveProject() const {
  return !project_catalog_.entries.empty() && HasProjectRoot();
}

std::size_t WorkspaceActionContext::ProjectCount() const {
  return project_catalog_.entries.size();
}

std::size_t WorkspaceActionContext::ActiveProjectIndex() const {
  return project_catalog_.active_index;
}

std::filesystem::path WorkspaceActionContext::ProjectRoot() const {
  return state_.root;
}

bool WorkspaceActionContext::OpenProject(const std::filesystem::path& project_root,
                                         bool restore_persistence,
                                         bool log_feedback) {
  return operations_.open_project(project_root, restore_persistence, log_feedback);
}

void WorkspaceActionContext::RequestCloseProject(std::size_t index) {
  operations_.request_close_project(index);
}

bool WorkspaceActionContext::SwitchProject(std::size_t index, bool log_feedback) {
  return operations_.switch_project(index, log_feedback);
}

ProjectOpenPickerResult WorkspaceActionContext::OpenNativeProjectPicker() {
  return operations_.open_native_project_picker();
}

bool WorkspaceActionContext::SidebarVisible() const {
  return state_.sidebar.visible;
}

bool WorkspaceActionContext::SidebarTemporary() const {
  return state_.sidebar.temporary;
}

std::string_view WorkspaceActionContext::SidebarViewId() const {
  return state_.sidebar.view_id;
}

SidebarMode WorkspaceActionContext::ActiveSidebarMode() const {
  return operations_.active_sidebar_mode();
}

void WorkspaceActionContext::ShowSidebarSurface() {
  state_.sidebar.visible = true;
  state_.surface.focus = FocusTarget::Sidebar;
}

void WorkspaceActionContext::ToggleSidebar() {
  operations_.toggle_sidebar();
}

void WorkspaceActionContext::CloseSidebar() {
  operations_.close_sidebar();
}

bool WorkspaceActionContext::ShowSidebarView(const SidebarViewInfo& view,
                                             const std::filesystem::path& root,
                                             const std::string& query) {
  switch (view.mode) {
    case SidebarMode::Tree:
      operations_.show_tree_sidebar(root);
      return true;
    case SidebarMode::Search:
      operations_.show_search_sidebar(query, false);
      return true;
    case SidebarMode::Chat:
      return false;
    case SidebarMode::Problems:
      operations_.show_problems_sidebar();
      return true;
    case SidebarMode::Git:
      operations_.show_git_sidebar();
      return true;
    case SidebarMode::Tests:
      operations_.show_tests_sidebar();
      return true;
    case SidebarMode::Outline: {
      if (operations_.get_setting_value) {
        const auto enabled = operations_.get_setting_value("editor.outline.enabled");
        if (enabled.has_value() &&
            (*enabled == "false" || *enabled == "0" || *enabled == "off")) {
          return false;
        }
      }
      operations_.show_outline_sidebar();
      return true;
    }
    case SidebarMode::Plugin:
      return operations_.show_plugin_sidebar(view.id, false);
    case SidebarMode::None:
      return false;
  }
  return false;
}

bool WorkspaceActionContext::ToggleSidebarView(const SidebarViewInfo& view,
                                               const std::filesystem::path& root,
                                               const std::string& query) {
  const bool same_view = state_.sidebar.visible && state_.sidebar.view_id == view.id;
  switch (view.mode) {
    case SidebarMode::Tree:
    case SidebarMode::Chat:
    case SidebarMode::Problems:
    case SidebarMode::Git:
    case SidebarMode::Tests:
    case SidebarMode::Outline:
    case SidebarMode::Plugin:
      if (same_view) {
        operations_.close_sidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::Search:
      if (same_view && !state_.sidebar.temporary) {
        operations_.close_sidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::None:
      return false;
  }
  return false;
}

float WorkspaceActionContext::CurrentWindowWidth() const {
  if (const std::optional<SDL_FRect> rect = operations_.current_window_rect(); rect.has_value()) {
    return rect->w;
  }
  return 1.0f;
}

void WorkspaceActionContext::SetSidebarWidth(float width) {
  state_.sidebar.width = ClampSidebarWidth(width, std::max(1.0f, CurrentWindowWidth()));
}

void WorkspaceActionContext::RefreshProjectFiles() {
  operations_.refresh_project_files();
}

void WorkspaceActionContext::ReloadCleanOpenBuffersFromDisk() {
  operations_.reload_clean_open_buffers_from_disk();
}

std::filesystem::path WorkspaceActionContext::TreeMutationBasePath(ActionSource source) const {
  return operations_.tree_mutation_base_path(source);
}

std::filesystem::path WorkspaceActionContext::ResolveTreeActionPath(ActionSource source) const {
  return operations_.resolve_tree_action_path(source);
}

std::filesystem::path WorkspaceActionContext::ActiveTabPath() const {
  return operations_.active_tab_path();
}

void WorkspaceActionContext::OpenCreatePathPrompt(bool directory,
                                                  const std::filesystem::path& base_path) {
  operations_.open_prompt_surface(directory ? PromptSurfaceState::Action::CreateDirectory
                                            : PromptSurfaceState::Action::CreateFile,
                                  PromptSurfaceState::Kind::TextInput, base_path, {});
}

void WorkspaceActionContext::OpenRenamePathPrompt(const std::filesystem::path& path) {
  operations_.open_prompt_surface(PromptSurfaceState::Action::RenamePath,
                                  PromptSurfaceState::Kind::TextInput, path,
                                  path.filename().string());
}

void WorkspaceActionContext::OpenDeletePathPrompt(const std::filesystem::path& path) {
  operations_.open_prompt_surface(PromptSurfaceState::Action::DeletePath,
                                  PromptSurfaceState::Kind::Confirm, path, {});
}

bool WorkspaceActionContext::WriteClipboardText(std::string_view text) const {
  return operations_.write_clipboard_text(text);
}

bool WorkspaceActionContext::WritePrimarySelectionText(std::string_view text) const {
  return operations_.write_primary_selection_text(text);
}

void WorkspaceActionContext::OpenTerminal(std::string command) {
  operations_.open_terminal(std::move(command));
}

void WorkspaceActionContext::ShowFileFinderWithQuery(std::string query) {
  state_.file_finder.SetIndex(&state_.file_index);
  state_.file_finder.SetQuery(std::move(query));
  operations_.show_overlay(OverlayMode::FileFinder);
}

void WorkspaceActionContext::ShowFileFinder() {
  operations_.show_overlay(OverlayMode::FileFinder);
  state_.file_finder.SetIndex(&state_.file_index);
  state_.file_finder.SetQuery("");
}

bool WorkspaceActionContext::OverlayVisible() const {
  return state_.overlay.visible;
}

void WorkspaceActionContext::DismissOverlay() {
  operations_.dismiss_overlay();
}

void WorkspaceActionContext::OpenSettingsOverlay() {
  operations_.open_settings_overlay();
}

void WorkspaceActionContext::OpenHelpAboutOverlay() {
  operations_.open_help_about_overlay();
}

void WorkspaceActionContext::ToggleStatusBar() {
  operations_.toggle_status_bar();
}

void WorkspaceActionContext::ToggleLayoutMode() {
  operations_.toggle_layout_mode();
}

void WorkspaceActionContext::ShowProjectSearchSidebar(std::string query) {
  operations_.show_search_sidebar(std::move(query), true);
}

bool WorkspaceActionContext::ShowCompletionOverlay(std::string* error_message) {
  return operations_.show_completion_overlay(error_message);
}

bool WorkspaceActionContext::ShowInsertSnippetOverlay(std::string* error_message) {
  return operations_.show_insert_snippet_overlay(error_message);
}

bool WorkspaceActionContext::ShowCodeActionsOverlay(std::string* error_message) {
  return operations_.show_code_actions_overlay(error_message);
}

bool WorkspaceActionContext::GoToLspDefinition(std::string* error_message) {
  return operations_.go_to_lsp_definition(error_message);
}

bool WorkspaceActionContext::FindLspReferences(std::string* error_message) {
  return operations_.find_lsp_references(error_message);
}

bool WorkspaceActionContext::DiscoverTestsForActiveBuffer(std::string* error_message) {
  return operations_.discover_tests_for_active_buffer(error_message);
}

bool WorkspaceActionContext::RunTests(const std::vector<std::string>& test_ids,
                                      std::string* error_message) {
  return operations_.run_tests(test_ids, error_message);
}

bool WorkspaceActionContext::RunAllDiscoveredTests(std::string* error_message) {
  return operations_.run_all_discovered_tests(error_message);
}

void WorkspaceActionContext::ShowOutputChannel(std::string_view id) {
  operations_.show_output_channel(id);
}

bool WorkspaceActionContext::RequestInlineCompletion(std::string* error_message) {
  return operations_.request_inline_completion(error_message);
}

bool WorkspaceActionContext::ActiveTabIsCompare() const {
  return state_.active_tab_index < state_.open_tabs.size() &&
         state_.open_tabs[state_.active_tab_index].kind == TabEntry::Kind::Compare;
}

bool WorkspaceActionContext::ActiveTabIsMerge() const {
  return state_.active_tab_index < state_.open_tabs.size() &&
         state_.open_tabs[state_.active_tab_index].kind == TabEntry::Kind::Merge;
}

void WorkspaceActionContext::OpenBufferSearch(std::string query) {
  operations_.open_buffer_search();
  state_.overlay.workflow.buffer_search.query.SetText(std::move(query));
  operations_.refresh_buffer_search();
}

void WorkspaceActionContext::OpenBufferReplace() {
  operations_.open_buffer_replace();
}

std::filesystem::path WorkspaceActionContext::ResolveComparePath(
    const std::filesystem::path& requested_path,
    ActionSource source) const {
  if (!requested_path.empty()) {
    return requested_path;
  }
  if (source == ActionSource::ContextMenu) {
    return operations_.resolve_tree_action_path(source);
  }
  if (const editor::TextViewport* viewport = operations_.active_editor_viewport();
      viewport != nullptr && !viewport->path().empty()) {
    return viewport->path().lexically_normal();
  }
  if (state_.sidebar.visible && operations_.active_sidebar_mode() == SidebarMode::Tree) {
    const auto& entries = state_.directory_tree.entries();
    if (state_.directory_tree.selected_index() < entries.size() &&
        !entries[state_.directory_tree.selected_index()].is_directory) {
      return entries[state_.directory_tree.selected_index()].path.lexically_normal();
    }
  }
  return {};
}

void WorkspaceActionContext::OpenComparePickerForPath(const std::filesystem::path& path,
                                                      const std::string& commit_spec) {
  operations_.open_compare_picker_for_path(path, commit_spec);
}

void WorkspaceActionContext::OpenHeadComparison(const std::filesystem::path& path) {
  state_.overlay.workflow.compare_picker.path = path.lexically_normal();
  operations_.open_comparison(project::GitCommitEntry{
      .hash = "HEAD",
      .short_hash = "HEAD",
      .subject = "HEAD",
  });
}

void WorkspaceActionContext::OpenMergeEditor(const std::filesystem::path& base_path,
                                             const std::filesystem::path& incoming_path,
                                             const std::filesystem::path& current_path,
                                             const std::filesystem::path& output_path) {
  operations_.open_merge_editor(base_path, incoming_path, current_path, output_path);
}

bool WorkspaceActionContext::OpenPath(const std::filesystem::path& path,
                                      std::string* error_message) {
  if (auto* editor_tab = operations_.active_editor_tab();
      editor_tab != nullptr && editor_tab->views.size() > 1) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      if (error_message != nullptr) {
        *error_message = "Failed to open file: " + path.string();
      }
      return false;
    }
    if (!operations_.replace_active_editor_view(opened_view)) {
      if (error_message != nullptr) {
        *error_message = "Failed to replace the active split with: " + path.string();
      }
      return false;
    }
    return true;
  }
  operations_.open_file(path);
  return true;
}

bool WorkspaceActionContext::OpenPathInNewTab(const std::filesystem::path& path) {
  return operations_.open_file_in_new_tab(path);
}

bool WorkspaceActionContext::OpenUntitledTab() {
  return operations_.open_untitled_tab();
}

std::optional<std::size_t> WorkspaceActionContext::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return operations_.find_tab_index_by_specifier(specifier, error_message);
}

void WorkspaceActionContext::ActivateTab(std::size_t index) {
  operations_.activate_tab(index);
}

bool WorkspaceActionContext::HasOpenTabs() const {
  return !state_.open_tabs.empty();
}

std::size_t WorkspaceActionContext::OpenTabCount() const {
  return state_.open_tabs.size();
}

std::size_t WorkspaceActionContext::ActiveTabIndex() const {
  return state_.active_tab_index;
}

void WorkspaceActionContext::MoveActiveTabTo(std::size_t index) {
  operations_.move_active_tab_to(index);
}

void WorkspaceActionContext::ReopenActiveTab() {
  operations_.reopen_active_tab();
}

bool WorkspaceActionContext::SaveTab(std::size_t index) {
  return operations_.save_tab(index);
}

void WorkspaceActionContext::ResetCaretBlink() {
  operations_.reset_caret_blink();
}

bool WorkspaceActionContext::OpenVerticalSplitPath(const std::filesystem::path& path,
                                                   std::string* error_message) {
  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(path)) {
    if (error_message != nullptr) {
      *error_message = "Failed to open file: " + path.string();
    }
    return false;
  }
  if (!operations_.split_active_editor(EditorSplitOrientation::Vertical)) {
    if (error_message != nullptr) {
      *error_message = "Failed to split the active editor";
    }
    return false;
  }
  if (!operations_.replace_active_editor_view(opened_view)) {
    if (error_message != nullptr) {
      *error_message = "Failed to replace the active split with: " + path.string();
    }
    return false;
  }
  return true;
}

void WorkspaceActionContext::SplitActiveEditorVertically() {
  operations_.split_active_editor(EditorSplitOrientation::Vertical);
}

void WorkspaceActionContext::UnsplitActiveEditor() {
  operations_.unsplit_active_editor();
}

void WorkspaceActionContext::CycleEditorSplit(int delta) {
  operations_.cycle_editor_split(delta);
}

void WorkspaceActionContext::ActivateOrderedEditorSplit(std::size_t index) {
  operations_.activate_ordered_editor_split(index);
}

std::size_t WorkspaceActionContext::ActiveEditorSplitCount() const {
  const auto* editor_tab = operations_.active_editor_tab();
  return editor_tab == nullptr ? 0 : editor_tab->views.size();
}

void WorkspaceActionContext::RequestCloseTab(std::size_t index) {
  operations_.request_close_tab(index);
}

void WorkspaceActionContext::RequestCloseTabs(std::vector<std::size_t> indices) {
  operations_.request_close_tabs(std::move(indices));
}

void WorkspaceActionContext::CloseAllTabs() {
  operations_.close_all_tabs();
}

bool WorkspaceActionContext::ExecuteLineNavigation(const LineNavigationRequest& request,
                                                   bool relative) {
  editor::TextViewport* viewport = operations_.active_navigable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const std::size_t line_count = std::max<std::size_t>(1, viewport->line_count());
  std::size_t line = 0;
  if (relative) {
    const long long current_line = static_cast<long long>(viewport->cursor_line()) + 1;
    const long long target_line = current_line + request.requested_line;
    line = static_cast<std::size_t>(
        std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
  } else if (request.requested_line > 0) {
    line = static_cast<std::size_t>(request.requested_line - 1);
  } else {
    const std::size_t from_end = static_cast<std::size_t>(-request.requested_line);
    line = from_end >= line_count ? 0 : line_count - from_end;
  }

  viewport->MoveCursorTo(line, request.column > 0 ? request.column - 1 : 0);
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_focused_editor_redraw();
  return true;
}

void WorkspaceActionContext::SelectAll() {
  if (operations_.select_all_at_active_single_line_text_surface()) {
    operations_.reset_caret_blink();
    return;
  }
  if (auto* viewport = operations_.active_navigable_viewport(); viewport != nullptr) {
    viewport->SelectAll();
    operations_.reset_caret_blink();
    operations_.request_focused_editor_redraw();
    state_.surface.focus = FocusTarget::Editor;
    return;
  }
}

void WorkspaceActionContext::Undo() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceActionContext::Undo");
  if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::size_t cursor_before_line = viewport->cursor_line();
    std::vector<std::string> before_lines;
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
    if (auto* merge_tab = operations_.active_merge_tab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      before_lines = viewport->lines();
      selection_before = viewport->selection_range();
      cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
    }
    bool changed = false;
    {
      util::PerformanceTrace::Scope scope("WorkspaceActionContext::Undo::ViewportUndo");
      changed = viewport->Undo();
    }
    if (changed && operations_.clear_active_snippet_session_after_undo) {
      operations_.clear_active_snippet_session_after_undo();
    }
    if (changed) {
      if (auto* compare_tab = operations_.active_compare_tab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Undo::RefreshCompareDerivedState");
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      }
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Undo::UpdateMergeTracking");
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                              selection_before, *cursor_before);
      }
      {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Undo::ResetCaretBlink");
        operations_.reset_caret_blink();
      }
      {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Undo::RequestTabRedraw");
        operations_.request_active_tab_redraw(false);
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Undo::RequestFocusedEditorRedraw");
        operations_.request_focused_editor_redraw();
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Undo::RequestLastChangeRedraw");
        operations_.request_active_editable_last_change_redraw();
      }
      if (viewport->dirty() != was_dirty) {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Undo::DirtyStateSideEffects");
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
  }
}

void WorkspaceActionContext::Redo() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceActionContext::Redo");
  if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::size_t cursor_before_line = viewport->cursor_line();
    std::vector<std::string> before_lines;
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
    if (auto* merge_tab = operations_.active_merge_tab();
        merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
      before_lines = viewport->lines();
      selection_before = viewport->selection_range();
      cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
    }
    bool changed = false;
    {
      util::PerformanceTrace::Scope scope("WorkspaceActionContext::Redo::ViewportRedo");
      changed = viewport->Redo();
    }
    if (changed) {
      if (auto* compare_tab = operations_.active_compare_tab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Redo::RefreshCompareDerivedState");
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      }
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Redo::UpdateMergeTracking");
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                              selection_before, *cursor_before);
      }
      {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Redo::ResetCaretBlink");
        operations_.reset_caret_blink();
      }
      {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Redo::RequestTabRedraw");
        operations_.request_active_tab_redraw(false);
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Redo::RequestFocusedEditorRedraw");
        operations_.request_focused_editor_redraw();
      }
      {
        util::PerformanceTrace::Scope scope(
            "WorkspaceActionContext::Redo::RequestLastChangeRedraw");
        operations_.request_active_editable_last_change_redraw();
      }
      if (viewport->dirty() != was_dirty) {
        util::PerformanceTrace::Scope scope("WorkspaceActionContext::Redo::DirtyStateSideEffects");
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
  }
}

std::string WorkspaceActionContext::CopySelectionText() const {
  if (state_.surface.focus == FocusTarget::Panel && operations_.terminal_has_selection()) {
    return operations_.selected_terminal_text();
  }
  if (operations_.has_selection_at_active_single_line_text_surface()) {
    return operations_.selected_text_at_active_single_line_text_surface();
  }
  if (const auto* viewport = operations_.active_navigable_viewport(); viewport != nullptr) {
    if (viewport->has_selection()) {
      return viewport->SelectedText();
    }
    return viewport->CurrentLineTextForClipboard();
  }
  return {};
}

std::optional<std::string> WorkspaceActionContext::LastTerminalCommandText() const {
  return operations_.last_terminal_command_text();
}

std::optional<std::string> WorkspaceActionContext::SelectionTextWithContext() const {
  return operations_.selection_text_with_context();
}

void WorkspaceActionContext::CutSelection() {
  if (operations_.cut_selection_at_active_single_line_text_surface()) {
    operations_.reset_caret_blink();
    return;
  }
  if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const bool has_selection = viewport->has_selection();
    const std::string text =
        has_selection ? viewport->SelectedText() : viewport->CurrentLineTextForClipboard();
    if (!text.empty() && operations_.write_clipboard_text(text)) {
      operations_.write_primary_selection_text(text);
      const std::size_t cursor_before_line = viewport->cursor_line();
      std::vector<std::string> before_lines;
      std::optional<editor::SelectionRange> selection_before;
      std::optional<editor::TextPosition> cursor_before;
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        before_lines = viewport->lines();
        selection_before = viewport->selection_range();
        cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
      }
      if (has_selection) {
        viewport->DeleteSelectedText();
      } else {
        viewport->DeleteCurrentLine();
      }
      if (auto* compare_tab = operations_.active_compare_tab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      }
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                              selection_before, *cursor_before);
      }
      operations_.reset_caret_blink();
      operations_.request_active_tab_redraw(false);
      operations_.request_focused_editor_redraw();
      operations_.request_active_editable_last_change_redraw();
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
  }
}

void WorkspaceActionContext::PasteClipboard() {
  if (const std::optional<std::string> clipboard_text = operations_.read_clipboard_text();
      clipboard_text.has_value()) {
    if (state_.surface.focus == FocusTarget::Panel && operations_.active_terminal_tab() != nullptr) {
      operations_.paste_clipboard_into_terminal();
      return;
    }
    if (operations_.insert_text_into_active_text_surface(*clipboard_text)) {
      return;
    }
    if (auto* viewport = operations_.active_editable_viewport(); viewport != nullptr) {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      std::vector<std::string> before_lines;
      std::optional<editor::SelectionRange> selection_before;
      std::optional<editor::TextPosition> cursor_before;
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        before_lines = viewport->lines();
        selection_before = viewport->selection_range();
        cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
      }
      viewport->InsertText(*clipboard_text);
      if (auto* compare_tab = operations_.active_compare_tab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      }
      if (auto* merge_tab = operations_.active_merge_tab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                              selection_before, *cursor_before);
      }
      operations_.reset_caret_blink();
      operations_.request_active_tab_redraw(false);
      operations_.request_focused_editor_redraw();
      operations_.request_active_editable_last_change_redraw();
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
  }
}

void WorkspaceActionContext::RefreshAvailableColorschemeNames() {
  operations_.refresh_available_colorscheme_names();
}

void WorkspaceActionContext::ApplyColorscheme(std::string_view name) {
  operations_.apply_colorscheme(name);
}

void WorkspaceActionContext::SetTabSize(std::size_t value) {
  state_.editor_preferences.tab_size = value;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

void WorkspaceActionContext::SetIndentWidth(std::size_t value) {
  state_.editor_preferences.indent_width = value;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

float WorkspaceActionContext::UiScale() const {
  return ui_scale_;
}

void WorkspaceActionContext::ApplyUiScale(float scale) {
  operations_.apply_ui_scale(scale);
}

void WorkspaceActionContext::SetSoftTabs(bool enabled) {
  state_.editor_preferences.soft_tabs = enabled;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

bool WorkspaceActionContext::SoftWrapEnabled() const {
  return state_.editor_preferences.soft_wrap;
}

void WorkspaceActionContext::SetSoftWrap(bool enabled) {
  state_.editor_preferences.soft_wrap = enabled;
  operations_.apply_editor_preferences_to_all_tabs();
  operations_.save_config_state();
}

editor::TextViewport* WorkspaceActionContext::ActiveEditableViewport() {
  return operations_.active_editable_viewport();
}

editor::TextViewport* WorkspaceActionContext::ActiveNavigableViewport() {
  return operations_.active_navigable_viewport();
}

TabEntry::EditorTabState* WorkspaceActionContext::ActiveEditorTab() {
  return operations_.active_editor_tab ? operations_.active_editor_tab() : nullptr;
}

editor::FoldingModel* WorkspaceActionContext::EnsureActiveFoldingModelFresh() {
  return operations_.ensure_active_folding_model_fresh
             ? operations_.ensure_active_folding_model_fresh()
             : nullptr;
}

void WorkspaceActionContext::NotifyEditorViewportChanged(bool last_change) {
  if (last_change) {
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      editor_tab->folding_model.MarkDirty();
    }
  }
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  operations_.request_focused_editor_redraw();
  if (last_change) {
    operations_.request_active_editable_last_change_redraw();
  }
}

void WorkspaceActionContext::NotifyEditorCaretMoved() {
  if (operations_.notify_snippet_session_caret_moved) {
    operations_.notify_snippet_session_caret_moved();
  }
  operations_.reset_caret_blink();
  operations_.request_active_tab_redraw(false);
  operations_.request_focused_editor_redraw();
}

std::optional<std::string> WorkspaceActionContext::GetSettingValue(std::string_view id) const {
  if (!operations_.get_setting_value) {
    return std::nullopt;
  }
  return operations_.get_setting_value(id);
}

namespace {

const char* CapabilitySettingKeyForToggle(ActionId id) {
  switch (id) {
    case ActionId::ToggleEditorFolding: return "editor.fold.enabled";
    case ActionId::ToggleEditorStickyScroll: return "editor.fold.sticky_scroll.enabled";
    case ActionId::ToggleEditorIndentGuides: return "editor.view.indent_guides.enabled";
    case ActionId::ToggleEditorRenderWhitespace: return "editor.view.render_whitespace";
    case ActionId::ToggleEditorOutline: return "editor.outline.enabled";
    case ActionId::ToggleEditorBracketMatchHighlight:
      return "editor.brackets.match_highlight.enabled";
    case ActionId::ToggleEditorAutoClosePairs: return "editor.brackets.auto_close.enabled";
    case ActionId::ToggleEditorSurround: return "editor.brackets.surround.enabled";
    case ActionId::ToggleEditorSmartIndent: return "editor.indent.smart.enabled";
    case ActionId::ToggleEditorToggleComment: return "editor.shaping.toggle_comment.enabled";
    case ActionId::ToggleEditorLineOps: return "editor.shaping.line_ops.enabled";
    case ActionId::ToggleEditorSortLines: return "editor.shaping.sort_lines.enabled";
    case ActionId::ToggleEditorAddCursorAtMatch:
      return "editor.multicursor.add_at_match.enabled";
    case ActionId::ToggleEditorOccurrencesHighlight: return "editor.occurrences.enabled";
    case ActionId::ToggleEditorSearchCaseSensitive: return "editor.search.case_sensitive";
    case ActionId::ToggleEditorSnippets: return "editor.snippets.enabled";
    case ActionId::ToggleEditorSaveTrim: return "editor.save.trim_trailing_whitespace";
    case ActionId::ToggleEditorSaveEnsureNewline: return "editor.save.ensure_final_newline";
    case ActionId::ToggleEditorAutoDetectIndent: return "editor.indent.detect_on_open";
    default: return nullptr;
  }
}

}  // namespace

void WorkspaceActionContext::ToggleEditorEssentialsCapability(ActionId id) {
  const char* key = CapabilitySettingKeyForToggle(id);
  if (key == nullptr) {
    return;
  }
  if (!operations_.set_setting_value || !operations_.get_setting_value) {
    return;
  }

  bool currently_enabled = true;
  if (const auto current = operations_.get_setting_value(key); current.has_value()) {
    currently_enabled = !(*current == "false" || *current == "0" || *current == "off");
  }
  const bool next_enabled = !currently_enabled;
  if (!operations_.set_setting_value(key, next_enabled ? "true" : "false")) {
    return;
  }
  if (id == ActionId::ToggleEditorOutline && operations_.normalize_sidebar_view_selection) {
    operations_.normalize_sidebar_view_selection();
  }
  operations_.request_active_tab_redraw(false);
}

bool WorkspaceActionContext::Focus(FocusRequestTarget target) {
  switch (target) {
    case FocusRequestTarget::Sidebar:
      if (state_.sidebar.visible) {
        state_.surface.focus = FocusTarget::Sidebar;
        return true;
      }
      return false;
    case FocusRequestTarget::Editor:
      state_.surface.focus = FocusTarget::Editor;
      return true;
    case FocusRequestTarget::Panel:
      if (state_.panel.command_mode || operations_.active_terminal_tab() != nullptr) {
        state_.surface.focus = FocusTarget::Panel;
        return true;
      }
      return false;
    case FocusRequestTarget::Unknown:
      return false;
  }
  return false;
}

}  // namespace microide::workspace
