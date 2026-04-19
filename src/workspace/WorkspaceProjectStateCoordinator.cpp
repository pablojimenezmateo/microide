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

WorkspaceShell::ProjectSurfaceState WorkspaceShell::CaptureProjectSurfaceState(
    const SurfaceState& state) {
  return ProjectSurfaceState{
      .sidebar_visible = state.sidebar_visible,
      .sidebar_mode = state.sidebar_mode,
      .sidebar_prev_mode = state.sidebar_prev_mode,
      .sidebar_plugin_id = state.sidebar_plugin_id,
      .sidebar_prev_plugin_id = state.sidebar_prev_plugin_id,
      .sidebar_temporary = state.sidebar_temporary,
      .overlay_visible = state.overlay_visible,
      .overlay_mode = state.overlay_mode,
      .buffer_search_field = state.buffer_search_field,
      .command_mode = state.command_mode,
      .focus = state.focus,
      .sidebar_width = state.sidebar_width,
      .bottom_panel_height = state.bottom_panel_height,
      .sidebar_scroll_row = state.sidebar_scroll_row,
      .overlay_scroll_row = state.overlay_scroll_row,
  };
}

void WorkspaceShell::ApplyProjectSurfaceState(const ProjectSurfaceState& state) {
  surface_.sidebar_visible = state.sidebar_visible;
  surface_.sidebar_mode = state.sidebar_mode;
  surface_.sidebar_prev_mode = state.sidebar_prev_mode;
  surface_.sidebar_plugin_id = state.sidebar_plugin_id;
  surface_.sidebar_prev_plugin_id = state.sidebar_prev_plugin_id;
  surface_.sidebar_temporary = state.sidebar_temporary;
  surface_.overlay_visible = state.overlay_visible;
  surface_.overlay_mode = state.overlay_mode;
  surface_.buffer_search_field = state.buffer_search_field;
  surface_.command_mode = state.command_mode;
  surface_.focus = state.focus;
  surface_.sidebar_width = state.sidebar_width;
  surface_.bottom_panel_height = state.bottom_panel_height;
  surface_.sidebar_scroll_row = state.sidebar_scroll_row;
  surface_.overlay_scroll_row = state.overlay_scroll_row;
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

  project_root_.clear();
  directory_tree_ = project::DirectoryTree{};
  file_index_ = project::FileIndex{};
  file_finder_ = project::FileFinder{};
  text_viewport_ = editor::TextViewport{};
  open_tabs_.clear();
  active_tab_index_ = 0;
  tab_scroll_index_ = 0;
  surface_.sidebar_visible = !show_welcome;
  surface_.sidebar_mode = SidebarMode::Tree;
  surface_.sidebar_prev_mode = SidebarMode::None;
  surface_.sidebar_plugin_id.clear();
  surface_.sidebar_prev_plugin_id.clear();
  surface_.sidebar_temporary = false;
  surface_.overlay_visible = false;
  surface_.overlay_mode = OverlayMode::FileFinder;
  surface_.buffer_search_field = BufferSearchField::Search;
  surface_.command_mode = false;
  surface_.focus = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  tab_drag_state_ = TabDragState{};
  surface_.sidebar_width = 288.0f;
  surface_.bottom_panel_height = 184.0f;
  surface_.sidebar_scroll_row = 0;
  surface_.overlay_scroll_row = 0;
  terminal_tabs_.clear();
  active_terminal_tab_index_ = 0;
  overlay_workflow_.buffer_search.query.clear();
  overlay_workflow_.buffer_search.replace_text.clear();
  overlay_workflow_.buffer_search.matches.clear();
  overlay_workflow_.buffer_search.selected_index = 0;
  overlay_workflow_.project_search.query.clear();
  overlay_workflow_.project_search.options = {};
  overlay_workflow_.project_search.edit_buffer.clear();
  overlay_workflow_.project_search.editing = false;
  overlay_workflow_.project_search.edit_field = ProjectSearchEditField::Query;
  overlay_workflow_.project_search.replace_text.clear();
  overlay_workflow_.project_search.results.clear();
  overlay_workflow_.project_search.selected_index = 0;
  overlay_workflow_.project_search.running = false;
  overlay_workflow_.project_search.truncated = false;
  overlay_workflow_.project_search.error.clear();
  git_sidebar_.entries.clear();
  git_sidebar_.base_ref.clear();
  git_sidebar_.base_label.clear();
  git_sidebar_.repo_available = false;
  git_sidebar_.selected_index = 0;
  problems_sidebar_.entries.clear();
  problems_sidebar_.selected_index = 0;
  plugin_sidebar_.items.clear();
  plugin_sidebar_.error.clear();
  plugin_sidebar_.selected_index = 0;
  diagnostics_store_.Clear();
  overlay_workflow_.compare_picker.path.clear();
  overlay_workflow_.compare_picker.query.clear();
  overlay_workflow_.compare_picker.commits.clear();
  overlay_workflow_.compare_picker.matches.clear();
  overlay_workflow_.compare_picker.selected_index = 0;
  command_.input.clear();
  command_.history.clear();
  command_.history_index.reset();
  command_.history_pending_input.clear();
  command_.feedback_text.clear();
  active_colorscheme_name_ = "default";
  project_base_color_ = std::nullopt;
  editor_preferences_ = EditorPreferences{};
  file_finder_.SetIndex(&file_index_);
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

  state.initialized = true;
  state.restore_persistence_on_activate = false;
  state.root = project_root_;
  state.directory_tree = std::move(directory_tree_);
  state.file_index = std::move(file_index_);
  state.file_finder = std::move(file_finder_);
  state.text_viewport = std::move(text_viewport_);
  state.open_tabs = std::move(open_tabs_);
  state.active_tab_index = active_tab_index_;
  state.tab_scroll_index = tab_scroll_index_;
  state.surface = CaptureProjectSurfaceState(surface_);
  state.terminal_tabs = std::move(terminal_tabs_);
  state.active_terminal_tab_index = active_terminal_tab_index_;
  state.overlay_workflow = std::move(overlay_workflow_);
  state.overlay_workflow.project_search.running = false;
  state.git_sidebar = std::move(git_sidebar_);
  state.problems_sidebar = std::move(problems_sidebar_);
  state.plugin_sidebar = std::move(plugin_sidebar_);
  state.diagnostics_store = std::move(diagnostics_store_);
  state.command = std::move(command_);
  state.active_colorscheme_name = active_colorscheme_name_;
  state.project_base_color = project_base_color_;
  state.editor_preferences = editor_preferences_;
  RebindProjectState(state);
}

void WorkspaceShell::LoadProjectState(ProjectWorkspaceState& state) {
  PersistenceCoordinator persistence(*this);
  StopProjectSearch();
  MenuCoordinator(*this).CloseTreeContextMenu();
  ClearEditorBlame();

  project_root_ = state.root;
  directory_tree_ = std::move(state.directory_tree);
  file_index_ = std::move(state.file_index);
  file_finder_ = std::move(state.file_finder);
  text_viewport_ = std::move(state.text_viewport);
  open_tabs_ = std::move(state.open_tabs);
  active_tab_index_ = state.active_tab_index;
  tab_scroll_index_ = state.tab_scroll_index;
  ApplyProjectSurfaceState(state.surface);
  terminal_tabs_ = std::move(state.terminal_tabs);
  active_terminal_tab_index_ = state.active_terminal_tab_index;
  overlay_workflow_ = std::move(state.overlay_workflow);
  overlay_workflow_.project_search.running = false;
  git_sidebar_ = std::move(state.git_sidebar);
  problems_sidebar_ = std::move(state.problems_sidebar);
  plugin_sidebar_ = std::move(state.plugin_sidebar);
  diagnostics_store_ = std::move(state.diagnostics_store);
  command_ = std::move(state.command);
  active_colorscheme_name_ = state.active_colorscheme_name;
  project_base_color_ = state.project_base_color;
  editor_preferences_ = state.editor_preferences;

  state.root = project_root_;
  file_finder_.SetIndex(&file_index_);
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
  surface_.sidebar_scroll_row = 0;
  RefreshGitSidebar();
  RefreshProblemsSidebar();

  if (surface_.sidebar_mode == SidebarMode::Search &&
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
