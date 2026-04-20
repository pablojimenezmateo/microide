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
  WorkspaceContext::RebindProjectState(state);
}

void WorkspaceShell::ClearDragState() {
  context_.interaction_state.drag_target = DragTarget::None;
  context_.interaction_state.drag_scrollbar_offset = 0.0f;
  context_.interaction_state.drag_editor_split_path.clear();
  context_.interaction_state.drag_editor_split_divider_index = 0;
}

void WorkspaceShell::ResetTransientInteractionState() {
  ClearDragState();
  context_.interaction_state.mouse_selecting = false;
}

void WorkspaceShell::ResetCurrentProjectStateStorage() {
  context_.ResetCurrentProjectStateStorage();
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
      context_.current_project_state.root.empty() ? std::filesystem::current_path(error) : context_.current_project_state.root;
  if (error || base_root.empty()) {
    return {};
  }

  const auto absolute_root = std::filesystem::absolute(base_root / project_root, error);
  return error ? std::filesystem::path{} : absolute_root.lexically_normal();
}

void WorkspaceShell::SetWelcomePlaceholder() {
  context_.current_project_state.text_viewport.SetPlaceholderText(
      "microide\n\n"
      "Welcome.\n"
      "Use File > New Project Tab... or run project-open.\n");
  ApplyEditorPreferences(context_.current_project_state.text_viewport);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  auto persistence = MakePersistenceCoordinator();
  StopProjectSearch();
  MakeMenuCoordinator().CloseTreeContextMenu();
  ClearEditorBlame();

  ResetCurrentProjectStateStorage();

  context_.current_project_state.sidebar.visible = !show_welcome;
  context_.current_project_state.surface.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  context_.interaction_state.tab_drag = TabDragState{};
  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferences(context_.current_project_state.text_viewport);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const std::filesystem::path& project_root,
                                              bool restore_persistence,
                                              bool log_feedback,
                                              bool activate_restored_tab) {
  (void) log_feedback;
  auto persistence = MakePersistenceCoordinator();
  util::StartupTrace::Scope trace_scope("WorkspaceShell::InitializeCurrentProject");
  ResetProjectScopedState(false);
  {
    util::StartupTrace::Scope set_root_scope("WorkspaceShell::SetProjectRoot");
    if (!SetProjectRoot(project_root)) {
      return false;
    }
  }

  context_.current_project_state.project_base_color = DefaultProjectBaseColor(context_.current_project_state.root);

  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferences(context_.current_project_state.text_viewport);
  if (restore_persistence) {
    persistence.RestoreConfigState();
  }

  if (restore_persistence && persistence.RestoreSessionState()) {
    if (context_.current_project_state.terminal_tabs.empty()) {
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
      context_.current_project_state.root / "docs" / "implementation-guide.md",
      context_.current_project_state.root / "README.md",
  };

  for (const auto& candidate : preferred_files) {
    editor::TextViewport startup_view;
    if (std::filesystem::exists(candidate) && startup_view.OpenFile(candidate)) {
      ApplyEditorPreferences(startup_view);
      context_.current_project_state.text_viewport = startup_view;
      context_.current_project_state.directory_tree.SelectPath(candidate);
      RevealSelectedTreeSidebarLine();
      context_.current_project_state.open_tabs.push_back(TabEntry{
          .kind = TabEntry::Kind::Editor,
          .path = candidate,
          .title = candidate.filename().string(),
          .editor_state = MakeEditorTabState(startup_view),
          .compare = std::nullopt,
          .merge = std::nullopt,
      });
      context_.current_project_state.active_tab_index = 0;
      if (context_.current_project_state.terminal_tabs.empty()) {
        OpenTerminal({}, false, false);
      }
      ReloadPluginsForCurrentProject();
      return true;
    }
  }

  context_.current_project_state.text_viewport.SetPlaceholderText(
      "microide\n\n"
      "Project loaded.\n"
      "Use the sidebar to open files.\n");
  ApplyEditorPreferences(context_.current_project_state.text_viewport);
  if (context_.current_project_state.terminal_tabs.empty()) {
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
  MakeMenuCoordinator().CloseTreeContextMenu();

  context_.current_project_state.initialized = true;
  context_.current_project_state.restore_persistence_on_activate = false;
  context_.current_project_state.overlay.workflow.project_search.running = false;
  state = std::move(context_.current_project_state);
  RebindProjectState(state);
  ResetCurrentProjectStateStorage();
}

void WorkspaceShell::LoadProjectState(ProjectWorkspaceState& state) {
  auto persistence = MakePersistenceCoordinator();
  StopProjectSearch();
  MakeMenuCoordinator().CloseTreeContextMenu();
  ClearEditorBlame();

  context_.current_project_state = std::move(state);
  context_.current_project_state.overlay.workflow.project_search.running = false;
  RebindProjectState(context_.current_project_state);
  ResetTransientInteractionState();

  state = ProjectWorkspaceState{};
  state.root = context_.current_project_state.root;
  state.initialized = true;
  state.restore_persistence_on_activate = false;
  persistence.ApplyColorscheme(context_.current_project_state.active_colorscheme_name, false, false);
  ApplyEditorPreferencesToAllTabs();
  if (context_.current_project_state.text_viewport.is_placeholder()) {
    ApplyEditorPreferences(context_.current_project_state.text_viewport);
  }
  if (!context_.current_project_state.root.empty() && context_.current_project_state.terminal_tabs.empty()) {
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
  context_.current_project_state.root = absolute_root.lexically_normal();
  {
    util::StartupTrace::Scope tree_scope("DirectoryTree::SetRoot");
    if (!context_.current_project_state.directory_tree.SetRoot(context_.current_project_state.root)) {
      return false;
    }
  }
  {
    util::StartupTrace::Scope index_scope("FileIndex::SetRoot");
    if (!context_.current_project_state.file_index.SetRoot(context_.current_project_state.root)) {
      return false;
    }
  }
  context_.current_project_state.file_finder.SetIndex(&context_.current_project_state.file_index);
  context_.current_project_state.sidebar.scroll_row = 0;
  RefreshGitSidebar();
  RefreshProblemsSidebar();

  if (ActiveSidebarMode() == SidebarMode::Search &&
      !context_.current_project_state.overlay.workflow.project_search.query.empty()) {
    RefreshProjectSearch();
  }
  return true;
}

std::filesystem::path WorkspaceShell::ConfigStatePath() const {
  return context_.current_project_state.root.empty() ? std::filesystem::path{} : ProjectStateDirectory() / "config";
}

std::filesystem::path WorkspaceShell::UserConfigPath() const {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "config";
}

std::filesystem::path WorkspaceShell::ProjectStateDirectory() const {
  if (context_.current_project_state.root.empty()) {
    return {};
  }
  const std::string directory_name = ProjectStateDirectoryName(context_.current_project_state.root);
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "projects" / directory_name;
}

}  // namespace microide::workspace
