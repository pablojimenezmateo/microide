#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include "platform/AppDirectories.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

bool WorkspaceShell::ConfigureProjectState(ProjectWorkspaceState& state,
                                           const std::filesystem::path& project_root) {
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(project_root, error);
  if (error || absolute_root.empty()) {
    return false;
  }

  state.root = absolute_root.lexically_normal();
  if (!state.directory_tree.SetRoot(state.root)) {
    return false;
  }
  if (!state.file_index.SetRoot(state.root)) {
    return false;
  }
  state.file_finder.SetIndex(&state.file_index);
  return true;
}

void WorkspaceShell::RebindProjectState(ProjectWorkspaceState& state) {
  state.file_finder.SetIndex(&state.file_index);
}

void WorkspaceShell::ClearDragState() {
  interaction_state_.drag_target = DragTarget::None;
  interaction_state_.drag_scrollbar_offset = 0.0f;
  interaction_state_.drag_editor_split_path.clear();
  interaction_state_.drag_editor_split_divider_index = 0;
}

void WorkspaceShell::ResetTransientInteractionState() {
  ClearDragState();
  interaction_state_.mouse_selecting = false;
}

void WorkspaceShell::ResetCurrentProjectStateStorage() {
  current_project_state_ = ProjectWorkspaceState{};
  RebindProjectState(current_project_state_);
  ResetTransientInteractionState();
}

std::filesystem::path WorkspaceShell::ResolveProjectRootInput(
    const std::filesystem::path& project_root) const {
  if (project_root.empty()) {
    return {};
  }

  std::error_code error;
  if (project_root.is_absolute()) {
    const auto absolute_root = std::filesystem::absolute(project_root, error);
    return error ? std::filesystem::path{} : absolute_root.lexically_normal();
  }

  const std::filesystem::path base_root =
      project_root_.empty() ? std::filesystem::current_path(error) : project_root_;
  if (error || base_root.empty()) {
    return {};
  }

  const auto absolute_root = std::filesystem::absolute(base_root / project_root, error);
  return error ? std::filesystem::path{} : absolute_root.lexically_normal();
}

void WorkspaceShell::SetWelcomePlaceholder() {
  text_viewport_.SetPlaceholderText(
      "microide\n\n"
      "Welcome.\n"
      "Use File > New Project Tab... or run project-open.\n");
  ApplyEditorPreferences(text_viewport_);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  PersistenceCoordinator persistence(*this);
  StopProjectSearch();
  MenuCoordinator(*this).CloseTreeContextMenu();
  ClearEditorBlame();

  ResetCurrentProjectStateStorage();

  sidebar_state_.visible = !show_welcome;
  surface_.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  tab_drag_state_ = TabDragState{};
  persistence.ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const std::filesystem::path& project_root,
                                              bool restore_persistence,
                                              bool log_feedback,
                                              bool activate_restored_tab) {
  (void) log_feedback;
  PersistenceCoordinator persistence(*this);
  util::StartupTrace::Scope trace_scope("WorkspaceShell::InitializeCurrentProject");
  ResetProjectScopedState(false);
  {
    util::StartupTrace::Scope set_root_scope("WorkspaceShell::SetProjectRoot");
    if (!SetProjectRoot(project_root)) {
      return false;
    }
  }

  project_base_color_ = DefaultProjectBaseColor(project_root_);

  persistence.ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (restore_persistence) {
    persistence.RestoreConfigState();
  }

  if (restore_persistence && persistence.RestoreSessionState()) {
    if (terminal_tabs_.empty()) {
      OpenTerminal({}, false, false);
    }
    ApplyEditorPreferencesToAllTabs();
    if (activate_restored_tab) {
      ActivateCurrentTabAfterStateLoad();
    }
    ReloadPluginsForCurrentProject();
    return true;
  }

  const std::vector<std::filesystem::path> preferred_files = {
      project_root_ / "docs" / "implementation-guide.md",
      project_root_ / "README.md",
  };

  for (const auto& candidate : preferred_files) {
    editor::TextViewport startup_view;
    if (std::filesystem::exists(candidate) && startup_view.OpenFile(candidate)) {
      ApplyEditorPreferences(startup_view);
      text_viewport_ = startup_view;
      directory_tree_.SelectPath(candidate);
      RevealSelectedTreeSidebarLine();
      open_tabs_.push_back(TabEntry{
          .kind = TabEntry::Kind::Editor,
          .path = candidate,
          .title = candidate.filename().string(),
          .editor_state = MakeEditorTabState(startup_view),
          .compare = std::nullopt,
          .merge = std::nullopt,
      });
      active_tab_index_ = 0;
      if (terminal_tabs_.empty()) {
        OpenTerminal({}, false, false);
      }
      ReloadPluginsForCurrentProject();
      return true;
    }
  }

  text_viewport_.SetPlaceholderText(
      "microide\n\n"
      "Project loaded.\n"
      "Use the sidebar to open files.\n");
  ApplyEditorPreferences(text_viewport_);
  if (terminal_tabs_.empty()) {
    OpenTerminal({}, false, false);
  }
  ReloadPluginsForCurrentProject();
  return true;
}

bool WorkspaceShell::ActivateProjectState(ProjectWorkspaceState& state,
                                          bool activate_restored_tab) {
  if (!state.initialized) {
    if (!InitializeCurrentProject(
            state.root, state.restore_persistence_on_activate, false, activate_restored_tab)) {
      return false;
    }
    state.initialized = true;
    state.restore_persistence_on_activate = false;
    return true;
  }

  LoadProjectState(state);
  if (activate_restored_tab) {
    ActivateCurrentTabAfterStateLoad();
  }
  ReloadPluginsForCurrentProject();
  return true;
}

void WorkspaceShell::StoreCurrentProjectState(ProjectWorkspaceState& state) {
  SyncActiveEditorTab();
  StopProjectSearch();
  MenuCoordinator(*this).CloseTreeContextMenu();

  current_project_state_.initialized = true;
  current_project_state_.restore_persistence_on_activate = false;
  current_project_state_.overlay.workflow.project_search.running = false;
  state = std::move(current_project_state_);
  RebindProjectState(state);
  ResetCurrentProjectStateStorage();
}

void WorkspaceShell::LoadProjectState(ProjectWorkspaceState& state) {
  PersistenceCoordinator persistence(*this);
  StopProjectSearch();
  MenuCoordinator(*this).CloseTreeContextMenu();
  ClearEditorBlame();

  current_project_state_ = std::move(state);
  current_project_state_.overlay.workflow.project_search.running = false;
  RebindProjectState(current_project_state_);
  ResetTransientInteractionState();

  state = ProjectWorkspaceState{};
  state.root = project_root_;
  state.initialized = true;
  state.restore_persistence_on_activate = false;
  persistence.ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferencesToAllTabs();
  if (text_viewport_.is_placeholder()) {
    ApplyEditorPreferences(text_viewport_);
  }
  if (!project_root_.empty() && terminal_tabs_.empty()) {
    OpenTerminal({}, false, false);
  }
}

bool WorkspaceShell::SetProjectRoot(const std::filesystem::path& project_root) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::SetProjectRoot");
  const std::filesystem::path absolute_root = ResolveProjectRootInput(project_root);
  if (absolute_root.empty()) {
    return false;
  }

  StopProjectSearch();
  project_root_ = absolute_root.lexically_normal();
  {
    util::StartupTrace::Scope tree_scope("DirectoryTree::SetRoot");
    if (!directory_tree_.SetRoot(project_root_)) {
      return false;
    }
  }
  {
    util::StartupTrace::Scope index_scope("FileIndex::SetRoot");
    if (!file_index_.SetRoot(project_root_)) {
      return false;
    }
  }
  file_finder_.SetIndex(&file_index_);
  sidebar_state_.scroll_row = 0;
  RefreshGitSidebar();
  RefreshProblemsSidebar();

  if (ActiveSidebarMode() == SidebarMode::Search &&
      !overlay_workflow_.project_search.query.empty()) {
    RefreshProjectSearch();
  }
  return true;
}

std::filesystem::path WorkspaceShell::ConfigStatePath() const {
  return project_root_.empty() ? std::filesystem::path{} : ProjectStateDirectory() / "config";
}

std::filesystem::path WorkspaceShell::UserConfigPath() const {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "config";
}

std::filesystem::path WorkspaceShell::ProjectStateDirectory() const {
  if (project_root_.empty()) {
    return {};
  }
  const std::string directory_name = ProjectStateDirectoryName(project_root_);
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "projects" / directory_name;
}

}  // namespace microide::workspace
