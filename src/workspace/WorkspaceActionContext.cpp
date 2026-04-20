#include "workspace/WorkspaceActionContext.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

WorkspaceActionContext::WorkspaceActionContext(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceActionContext::PrepareForAction(ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    MenuCoordinator(shell_).CloseTreeContextMenu();
  }
}

bool WorkspaceActionContext::RejectAction(ActionSource source, std::string feedback) {
  return CommandPromptCoordinator(shell_).RejectAction(source, std::move(feedback));
}

SidebarViewRequest WorkspaceActionContext::ParseSidebarViewRequest(
    const std::vector<std::string>& args) const {
  return workspace::ParseSidebarViewRequest(args, shell_.plugin_runtime_.Host());
}

bool WorkspaceActionContext::HasProjectRoot() const {
  return !shell_.project_root_.empty();
}

bool WorkspaceActionContext::HasActiveProject() const {
  return !shell_.project_catalog_.entries.empty() && HasProjectRoot();
}

std::size_t WorkspaceActionContext::ProjectCount() const {
  return shell_.project_catalog_.entries.size();
}

std::size_t WorkspaceActionContext::ActiveProjectIndex() const {
  return shell_.project_catalog_.active_index;
}

std::filesystem::path WorkspaceActionContext::ProjectRoot() const {
  return shell_.project_root_;
}

bool WorkspaceActionContext::OpenProject(const std::filesystem::path& project_root,
                                         bool restore_persistence,
                                         bool log_feedback) {
  return shell_.OpenProjectTab(project_root, restore_persistence, log_feedback);
}

void WorkspaceActionContext::RequestCloseProject(std::size_t index) {
  shell_.RequestCloseProject(index);
}

bool WorkspaceActionContext::SwitchProject(std::size_t index, bool log_feedback) {
  return shell_.SwitchProject(index, log_feedback);
}

ProjectOpenPickerResult WorkspaceActionContext::OpenNativeProjectPicker() {
  switch (shell_.OpenNativeProjectPicker(nullptr)) {
    case WorkspaceShell::ProjectOpenDialogLaunchResult::Launched:
      return ProjectOpenPickerResult::Launched;
    case WorkspaceShell::ProjectOpenDialogLaunchResult::AlreadyOpen:
      return ProjectOpenPickerResult::AlreadyOpen;
    case WorkspaceShell::ProjectOpenDialogLaunchResult::Unavailable:
      return ProjectOpenPickerResult::Unavailable;
  }
  return ProjectOpenPickerResult::Unavailable;
}

bool WorkspaceActionContext::SidebarVisible() const {
  return shell_.sidebar_state_.visible;
}

bool WorkspaceActionContext::SidebarTemporary() const {
  return shell_.sidebar_state_.temporary;
}

std::string_view WorkspaceActionContext::SidebarViewId() const {
  return shell_.sidebar_state_.view_id;
}

SidebarMode WorkspaceActionContext::ActiveSidebarMode() const {
  return shell_.ActiveSidebarMode();
}

void WorkspaceActionContext::ShowSidebarSurface() {
  shell_.sidebar_state_.visible = true;
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
}

void WorkspaceActionContext::ToggleSidebar() {
  shell_.ToggleSidebar();
}

void WorkspaceActionContext::CloseSidebar() {
  shell_.CloseSidebar();
}

bool WorkspaceActionContext::ShowSidebarView(const SidebarViewInfo& view,
                                             const std::filesystem::path& root,
                                             const std::string& query) {
  switch (view.mode) {
    case SidebarMode::Tree:
      shell_.ShowTreeSidebar(root);
      return true;
    case SidebarMode::Search:
      shell_.ShowSearchSidebar(query, false);
      return true;
    case SidebarMode::Problems:
      shell_.ShowProblemsSidebar();
      return true;
    case SidebarMode::Git:
      shell_.ShowGitSidebar();
      return true;
    case SidebarMode::Plugin:
      return shell_.ShowPluginSidebar(view.id, false);
    case SidebarMode::None:
      return false;
  }
  return false;
}

bool WorkspaceActionContext::ToggleSidebarView(const SidebarViewInfo& view,
                                               const std::filesystem::path& root,
                                               const std::string& query) {
  const bool same_view = shell_.sidebar_state_.visible && shell_.sidebar_state_.view_id == view.id;
  switch (view.mode) {
    case SidebarMode::Tree:
    case SidebarMode::Problems:
    case SidebarMode::Git:
    case SidebarMode::Plugin:
      if (same_view) {
        shell_.CloseSidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::Search:
      if (same_view && !shell_.sidebar_state_.temporary) {
        shell_.CloseSidebar();
        return true;
      }
      return ShowSidebarView(view, root, query);
    case SidebarMode::None:
      return false;
  }
  return false;
}

float WorkspaceActionContext::CurrentWindowWidth() const {
  if (const std::optional<SDL_FRect> rect = shell_.CurrentWindowRect(); rect.has_value()) {
    return rect->w;
  }
  return 1.0f;
}

void WorkspaceActionContext::SetSidebarWidth(float width) {
  shell_.sidebar_state_.width = ClampSidebarWidth(width, std::max(1.0f, CurrentWindowWidth()));
}

void WorkspaceActionContext::RefreshProjectFiles() {
  shell_.RefreshProjectFiles();
}

void WorkspaceActionContext::ReloadCleanOpenBuffersFromDisk() {
  shell_.ReloadCleanOpenBuffersFromDisk();
}

std::filesystem::path WorkspaceActionContext::TreeMutationBasePath(ActionSource source) const {
  return shell_.TreeMutationBasePath(source);
}

std::filesystem::path WorkspaceActionContext::ResolveTreeActionPath(ActionSource source) const {
  return shell_.ResolveTreeActionPath(source);
}

void WorkspaceActionContext::OpenCreatePathPrompt(bool directory,
                                                  const std::filesystem::path& base_path) {
  shell_.OpenPromptSurface(directory ? WorkspaceShell::PromptSurfaceState::Action::CreateDirectory
                                     : WorkspaceShell::PromptSurfaceState::Action::CreateFile,
                           WorkspaceShell::PromptSurfaceState::Kind::TextInput, base_path);
}

void WorkspaceActionContext::OpenRenamePathPrompt(const std::filesystem::path& path) {
  shell_.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                           WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                           path.filename().string());
}

void WorkspaceActionContext::OpenDeletePathPrompt(const std::filesystem::path& path) {
  shell_.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                           WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
}

bool WorkspaceActionContext::WriteClipboardText(std::string_view text) const {
  return shell_.WriteClipboardText(text);
}

bool WorkspaceActionContext::WritePrimarySelectionText(std::string_view text) const {
  return shell_.WritePrimarySelectionText(text);
}

void WorkspaceActionContext::OpenTerminal(std::string command) {
  shell_.OpenTerminal(std::move(command));
}

void WorkspaceActionContext::ShowFileFinderWithQuery(std::string query) {
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  shell_.file_finder_.SetQuery(std::move(query));
  shell_.ShowOverlay(WorkspaceShell::OverlayMode::FileFinder);
}

void WorkspaceActionContext::ShowFileFinder() {
  shell_.ShowOverlay(WorkspaceShell::OverlayMode::FileFinder);
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  shell_.file_finder_.SetQuery("");
}

bool WorkspaceActionContext::OverlayVisible() const {
  return shell_.overlay_state_.visible;
}

void WorkspaceActionContext::DismissOverlay() {
  shell_.DismissOverlay();
}

void WorkspaceActionContext::ShowProjectSearchSidebar(std::string query) {
  shell_.ShowSearchSidebar(std::move(query), true);
}

bool WorkspaceActionContext::ActiveTabIsCompare() const {
  return shell_.ActiveTabIsCompare();
}

bool WorkspaceActionContext::ActiveTabIsMerge() const {
  return shell_.ActiveTabIsMerge();
}

void WorkspaceActionContext::OpenBufferSearch(std::string query) {
  shell_.OpenBufferSearch();
  shell_.overlay_workflow_.buffer_search.query = std::move(query);
  shell_.RefreshBufferSearch();
}

void WorkspaceActionContext::OpenBufferReplace() {
  shell_.OpenBufferReplace();
}

std::filesystem::path WorkspaceActionContext::ResolveComparePath(
    const std::filesystem::path& requested_path,
    ActionSource source) const {
  if (!requested_path.empty()) {
    return requested_path;
  }
  if (source == ActionSource::ContextMenu) {
    return shell_.ResolveTreeActionPath(source);
  }
  if (!shell_.text_viewport_.path().empty()) {
    return shell_.text_viewport_.path().lexically_normal();
  }
  if (shell_.sidebar_state_.visible && shell_.ActiveSidebarMode() == SidebarMode::Tree) {
    const auto& entries = shell_.directory_tree_.entries();
    if (shell_.directory_tree_.selected_index() < entries.size() &&
        !entries[shell_.directory_tree_.selected_index()].is_directory) {
      return entries[shell_.directory_tree_.selected_index()].path.lexically_normal();
    }
  }
  return {};
}

void WorkspaceActionContext::OpenComparePickerForPath(const std::filesystem::path& path,
                                                      const std::string& commit_spec) {
  shell_.OpenComparePickerForPath(path, commit_spec);
}

void WorkspaceActionContext::OpenHeadComparison(const std::filesystem::path& path) {
  shell_.overlay_workflow_.compare_picker.path = path.lexically_normal();
  shell_.OpenComparison(project::GitCommitEntry{
      .hash = "HEAD",
      .short_hash = "HEAD",
      .subject = "HEAD",
  });
}

void WorkspaceActionContext::OpenMergeEditor(const std::filesystem::path& base_path,
                                             const std::filesystem::path& incoming_path,
                                             const std::filesystem::path& current_path,
                                             const std::filesystem::path& output_path) {
  shell_.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
}

bool WorkspaceActionContext::OpenPath(const std::filesystem::path& path,
                                      std::string* error_message) {
  if (auto* editor_tab = shell_.ActiveEditorTab();
      editor_tab != nullptr && editor_tab->views.size() > 1) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      if (error_message != nullptr) {
        *error_message = "Failed to open file: " + path.string();
      }
      return false;
    }
    if (!shell_.ReplaceActiveEditorView(opened_view)) {
      if (error_message != nullptr) {
        *error_message = "Failed to replace the active split with: " + path.string();
      }
      return false;
    }
    return true;
  }
  shell_.OpenFile(path);
  return true;
}

bool WorkspaceActionContext::OpenPathInNewTab(const std::filesystem::path& path) {
  return shell_.OpenFileInNewTab(path);
}

bool WorkspaceActionContext::OpenUntitledTab() {
  return shell_.OpenUntitledTab();
}

std::optional<std::size_t> WorkspaceActionContext::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return shell_.FindTabIndexBySpecifier(specifier, error_message);
}

void WorkspaceActionContext::ActivateTab(std::size_t index) {
  shell_.ActivateTab(index);
}

bool WorkspaceActionContext::HasOpenTabs() const {
  return !shell_.open_tabs_.empty();
}

std::size_t WorkspaceActionContext::OpenTabCount() const {
  return shell_.open_tabs_.size();
}

std::size_t WorkspaceActionContext::ActiveTabIndex() const {
  return shell_.active_tab_index_;
}

void WorkspaceActionContext::MoveActiveTabTo(std::size_t index) {
  shell_.MoveActiveTabTo(index);
}

void WorkspaceActionContext::ReopenActiveTab() {
  shell_.ReopenActiveTab();
}

bool WorkspaceActionContext::SaveTab(std::size_t index) {
  return shell_.SaveTab(index);
}

void WorkspaceActionContext::ResetCaretBlink() {
  shell_.ResetCaretBlink();
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
  if (!shell_.SplitActiveEditor(WorkspaceShell::EditorSplitOrientation::Vertical)) {
    if (error_message != nullptr) {
      *error_message = "Failed to split the active editor";
    }
    return false;
  }
  if (!shell_.ReplaceActiveEditorView(opened_view)) {
    if (error_message != nullptr) {
      *error_message = "Failed to replace the active split with: " + path.string();
    }
    return false;
  }
  return true;
}

void WorkspaceActionContext::SplitActiveEditorVertically() {
  shell_.SplitActiveEditor(WorkspaceShell::EditorSplitOrientation::Vertical);
}

void WorkspaceActionContext::UnsplitActiveEditor() {
  shell_.UnsplitActiveEditor();
}

void WorkspaceActionContext::CycleEditorSplit(int delta) {
  shell_.CycleEditorSplit(delta);
}

void WorkspaceActionContext::ActivateOrderedEditorSplit(std::size_t index) {
  shell_.ActivateOrderedEditorSplit(index);
}

std::size_t WorkspaceActionContext::ActiveEditorSplitCount() const {
  const auto* editor_tab = shell_.ActiveEditorTab();
  return editor_tab == nullptr ? 0 : editor_tab->views.size();
}

void WorkspaceActionContext::RequestCloseTab(std::size_t index) {
  shell_.RequestCloseTab(index);
}

void WorkspaceActionContext::RequestCloseTabs(std::vector<std::size_t> indices) {
  shell_.RequestCloseTabs(std::move(indices));
}

void WorkspaceActionContext::CloseAllTabs() {
  shell_.CloseAllTabs();
}

bool WorkspaceActionContext::ExecuteLineNavigation(const LineNavigationRequest& request,
                                                   bool relative) {
  const std::size_t line_count = std::max<std::size_t>(1, shell_.text_viewport_.line_count());
  std::size_t line = 0;
  if (relative) {
    const long long current_line = static_cast<long long>(shell_.text_viewport_.cursor_line()) + 1;
    const long long target_line = current_line + request.requested_line;
    line = static_cast<std::size_t>(
        std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
  } else if (request.requested_line > 0) {
    line = static_cast<std::size_t>(request.requested_line - 1);
  } else {
    const std::size_t from_end = static_cast<std::size_t>(-request.requested_line);
    line = from_end >= line_count ? 0 : line_count - from_end;
  }

  shell_.text_viewport_.MoveCursorTo(line, request.column > 0 ? request.column - 1 : 0);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  shell_.RequestFocusedEditorRedraw();
  return true;
}

void WorkspaceActionContext::SelectAll() {
  if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
    viewport->SelectAll();
    shell_.ResetCaretBlink();
    shell_.RequestFocusedEditorRedraw();
  }
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
}

void WorkspaceActionContext::Undo() {
  if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    if (viewport->Undo()) {
      if (auto* compare_tab = shell_.ActiveCompareTab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        shell_.RefreshCompareTabDerivedState(*compare_tab);
        shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      }
      if (auto* merge_tab = shell_.ActiveMergeTab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                    cursor_before);
      }
      shell_.ResetCaretBlink();
      shell_.RequestActiveTabRedraw(false);
      shell_.RequestFocusedEditorRedraw();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                            viewport->cursor_line());
        shell_.RequestTabStripRedraw();
      }
    }
  }
}

void WorkspaceActionContext::Redo() {
  if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::vector<std::string> before_lines = viewport->lines();
    const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
    const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
    if (viewport->Redo()) {
      if (auto* compare_tab = shell_.ActiveCompareTab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        shell_.RefreshCompareTabDerivedState(*compare_tab);
        shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      }
      if (auto* merge_tab = shell_.ActiveMergeTab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                    cursor_before);
      }
      shell_.ResetCaretBlink();
      shell_.RequestActiveTabRedraw(false);
      shell_.RequestFocusedEditorRedraw();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                            viewport->cursor_line());
        shell_.RequestTabStripRedraw();
      }
    }
  }
}

std::string WorkspaceActionContext::CopySelectionText() const {
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Panel && shell_.TerminalHasSelection()) {
    return shell_.SelectedTerminalText();
  }
  if (const auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
    return viewport->SelectedText();
  }
  return {};
}

std::optional<std::string> WorkspaceActionContext::LastTerminalCommandText() const {
  return shell_.LastTerminalCommandText();
}

std::optional<std::string> WorkspaceActionContext::SelectionTextWithContext() const {
  return shell_.SelectionTextWithContext();
}

void WorkspaceActionContext::CutSelection() {
  if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
    const bool was_dirty = viewport->dirty();
    const std::string text = viewport->SelectedText();
    if (!text.empty() && shell_.WriteClipboardText(text)) {
      shell_.WritePrimarySelectionText(text);
      const std::vector<std::string> before_lines = viewport->lines();
      const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
      const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
      viewport->DeleteSelectedText();
      if (auto* compare_tab = shell_.ActiveCompareTab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        shell_.RefreshCompareTabDerivedState(*compare_tab);
        shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      }
      if (auto* merge_tab = shell_.ActiveMergeTab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                    cursor_before);
      }
      shell_.ResetCaretBlink();
      shell_.RequestActiveTabRedraw(false);
      shell_.RequestFocusedEditorRedraw();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                            viewport->cursor_line());
        shell_.RequestTabStripRedraw();
      }
    }
  }
}

void WorkspaceActionContext::PasteClipboard() {
  if (const std::optional<std::string> clipboard_text = shell_.ReadClipboardText();
      clipboard_text.has_value()) {
    if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Panel &&
        shell_.ActiveTerminalTab() != nullptr) {
      TextInputCoordinator(shell_).PasteClipboardIntoTerminal();
      return;
    }
    if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
      const bool was_dirty = viewport->dirty();
      const std::vector<std::string> before_lines = viewport->lines();
      const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
      const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
      viewport->InsertText(*clipboard_text);
      if (auto* compare_tab = shell_.ActiveCompareTab();
          compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
        shell_.RefreshCompareTabDerivedState(*compare_tab);
        shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      }
      if (auto* merge_tab = shell_.ActiveMergeTab();
          merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
        shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                    cursor_before);
      }
      shell_.ResetCaretBlink();
      shell_.RequestActiveTabRedraw(false);
      shell_.RequestFocusedEditorRedraw();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                            viewport->cursor_line());
        shell_.RequestTabStripRedraw();
      }
    }
  }
}

void WorkspaceActionContext::RefreshAvailableColorschemeNames() {
  PersistenceCoordinator(shell_).RefreshAvailableColorschemeNames();
}

void WorkspaceActionContext::ApplyColorscheme(std::string_view name) {
  PersistenceCoordinator(shell_).ApplyColorscheme(name, true, true);
}

void WorkspaceActionContext::SetTabSize(std::size_t value) {
  shell_.editor_preferences_.tab_size = value;
  shell_.ApplyEditorPreferencesToAllTabs();
  PersistenceCoordinator(shell_).SaveConfigState();
}

void WorkspaceActionContext::SetIndentWidth(std::size_t value) {
  shell_.editor_preferences_.indent_width = value;
  shell_.ApplyEditorPreferencesToAllTabs();
  PersistenceCoordinator(shell_).SaveConfigState();
}

float WorkspaceActionContext::UiScale() const {
  return shell_.ui_scale_;
}

void WorkspaceActionContext::ApplyUiScale(float scale) {
  PersistenceCoordinator(shell_).ApplyUiScale(scale, true, true);
}

void WorkspaceActionContext::SetSoftTabs(bool enabled) {
  shell_.editor_preferences_.soft_tabs = enabled;
  shell_.ApplyEditorPreferencesToAllTabs();
  PersistenceCoordinator(shell_).SaveConfigState();
}

bool WorkspaceActionContext::Focus(FocusRequestTarget target) {
  switch (target) {
    case FocusRequestTarget::Sidebar:
      if (shell_.sidebar_state_.visible) {
        shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
        return true;
      }
      return false;
    case FocusRequestTarget::Editor:
      shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
      return true;
    case FocusRequestTarget::Panel:
      if (shell_.panel_state_.command_mode || shell_.ActiveTerminalTab() != nullptr) {
        shell_.surface_.focus = WorkspaceShell::FocusTarget::Panel;
        return true;
      }
      return false;
    case FocusRequestTarget::Unknown:
      return false;
  }
  return false;
}

void WorkspaceActionContext::OpenCommandPrompt(std::string input) {
  const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
  shell_.panel_state_.command_mode = true;
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Panel;
  shell_.command_.input = std::move(input);
  CommandPromptCoordinator(shell_).ResetSessionState();
  shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
}

bool WorkspaceActionContext::PluginRuntimeEnabled() const {
  return shell_.plugin_runtime_.enabled();
}

void WorkspaceActionContext::ReloadPluginsWithFeedback() {
  shell_.ReloadPluginsForCurrentProject();
  CommandPromptCoordinator(shell_).SetFeedback(shell_.PluginRuntimeReloadSummary());
}

void WorkspaceActionContext::RequestQuit() {
  shell_.RequestQuit();
}

}  // namespace microide::workspace
