#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include "util/StartupTrace.h"
#include "workspace/WorkspaceShellShared.h"

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

WorkspaceShell::ProjectOpenDialogLaunchResult WorkspaceShell::OpenNativeProjectPicker(
    std::string* error_message) {
  if (project_open_dialog_active_) {
    if (error_message != nullptr) {
      *error_message = "Project picker already open";
    }
    return ProjectOpenDialogLaunchResult::AlreadyOpen;
  }

  std::error_code error;
  const std::filesystem::path default_location =
      project_root_.empty() ? std::filesystem::current_path(error) : project_root_;
  const std::filesystem::path normalized_default = error ? std::filesystem::path{}
                                                         : default_location.lexically_normal();

  project_open_dialog_active_ = true;
  if (project_open_dialog_launcher_) {
    if (!project_open_dialog_launcher_(*this, normalized_default)) {
      project_open_dialog_active_ = false;
      if (error_message != nullptr) {
        *error_message = "Native dialog backend unavailable";
      }
      return ProjectOpenDialogLaunchResult::Unavailable;
    }
    return ProjectOpenDialogLaunchResult::Launched;
  }

  const std::string default_location_string = normalized_default.string();
  SDL_ClearError();
  SDL_ShowOpenFolderDialog(&WorkspaceShell::OnProjectOpenDialogComplete, this, dialog_window_,
                           default_location_string.empty() ? nullptr
                                                           : default_location_string.c_str(),
                           false);
  const std::string dialog_error = SDL_GetError();
  if (!dialog_error.empty()) {
    project_open_dialog_active_ = false;
    if (error_message != nullptr) {
      *error_message = dialog_error;
    }
    return ProjectOpenDialogLaunchResult::Unavailable;
  }

  return ProjectOpenDialogLaunchResult::Launched;
}

void SDLCALL WorkspaceShell::OnProjectOpenDialogComplete(void* userdata,
                                                         const char* const* filelist,
                                                         int /*filter*/) {
  auto* shell = static_cast<WorkspaceShell*>(userdata);
  if (shell == nullptr) {
    return;
  }

  PendingProjectOpenDialogResult pending;
  pending.ready = true;
  if (filelist == nullptr) {
    pending.error_message = SDL_GetError();
  } else if (filelist[0] == nullptr) {
    pending.cancelled = true;
  } else {
    pending.selected_path = std::filesystem::path(filelist[0]).lexically_normal();
  }

  {
    std::lock_guard<std::mutex> lock(shell->project_open_dialog_mutex_);
    shell->pending_project_open_dialog_result_ = std::move(pending);
  }

  if (shell->project_open_dialog_event_type_ != 0) {
    SDL_Event event{};
    event.type = shell->project_open_dialog_event_type_;
    SDL_PushEvent(&event);
  }
}

void WorkspaceShell::ConsumePendingProjectOpenDialogResult() {
  PendingProjectOpenDialogResult pending;
  {
    std::lock_guard<std::mutex> lock(project_open_dialog_mutex_);
    if (!pending_project_open_dialog_result_.ready) {
      return;
    }
    pending = std::move(pending_project_open_dialog_result_);
    pending_project_open_dialog_result_ = PendingProjectOpenDialogResult{};
  }

  project_open_dialog_active_ = false;
  if (!pending.error_message.empty()) {
    LogMessage("Project picker failed: " + pending.error_message);
    return;
  }
  if (pending.cancelled) {
    LogMessage("Open project cancelled");
    return;
  }
  if (pending.selected_path.empty()) {
    LogMessage("Project picker returned no folder");
    return;
  }

  OpenProjectTab(pending.selected_path, true, true);
}

void WorkspaceShell::SetWelcomePlaceholder() {
  text_viewport_.SetPlaceholderText(
      "microide\n\n"
      "Welcome.\n"
      "Use File > New Project Tab... or run project-open.\n");
  ApplyEditorPreferences(text_viewport_);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  StopProjectSearch();
  CloseTreeContextMenu();
  ClearEditorBlame();

  project_root_.clear();
  directory_tree_ = project::DirectoryTree{};
  file_index_ = project::FileIndex{};
  file_finder_ = project::FileFinder{};
  text_viewport_ = editor::TextViewport{};
  open_tabs_.clear();
  active_tab_index_ = 0;
  tab_scroll_index_ = 0;
  sidebar_visible_ = !show_welcome;
  sidebar_mode_ = SidebarMode::Tree;
  sidebar_prev_mode_ = SidebarMode::None;
  sidebar_temporary_ = false;
  overlay_visible_ = false;
  overlay_mode_ = OverlayMode::FileFinder;
  buffer_search_field_ = BufferSearchField::Search;
  command_mode_ = false;
  focus_ = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  tab_drag_state_ = TabDragState{};
  sidebar_width_ = 288.0f;
  bottom_panel_height_ = 184.0f;
  sidebar_scroll_row_ = 0;
  overlay_scroll_row_ = 0;
  terminal_tabs_.clear();
  active_terminal_tab_index_ = 0;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  project_search_query_.clear();
  project_search_options_ = {};
  project_search_edit_buffer_.clear();
  project_search_editing_ = false;
  project_search_edit_field_ = ProjectSearchEditField::Query;
  project_replace_text_.clear();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_search_running_ = false;
  project_search_truncated_ = false;
  project_search_error_.clear();
  git_sidebar_entries_.clear();
  git_base_ref_.clear();
  git_base_label_.clear();
  git_repo_available_ = false;
  git_sidebar_selected_index_ = 0;
  project_search_run_id_ = 0;
  compare_picker_path_.clear();
  compare_picker_query_.clear();
  compare_picker_commits_.clear();
  compare_picker_matches_.clear();
  compare_picker_selected_index_ = 0;
  command_input_.clear();
  command_history_.clear();
  command_history_index_.reset();
  command_history_pending_input_.clear();
  command_completion_feedback_.clear();
  status_message_.clear();
  active_colorscheme_name_ = "default";
  project_base_color_ = std::nullopt;
  editor_preferences_ = EditorPreferences{};
  file_finder_.SetIndex(&file_index_);
  ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const std::filesystem::path& project_root,
                                              bool restore_persistence,
                                              bool log_feedback,
                                              bool activate_restored_tab) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::InitializeCurrentProject");
  ResetProjectScopedState(false);
  {
    util::StartupTrace::Scope set_root_scope("WorkspaceShell::SetProjectRoot");
    if (!SetProjectRoot(project_root)) {
      return false;
    }
  }

  project_base_color_ = DefaultProjectBaseColor(project_root_);

  ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (log_feedback) {
    LogMessage("Project loaded: " + project_root_.lexically_normal().string());
  }
  if (restore_persistence && RestoreConfigState() && log_feedback) {
    LogMessage("Restored editor preferences and colorscheme");
  }

  if (restore_persistence && RestoreSessionState()) {
    if (terminal_tabs_.empty()) {
      OpenTerminal({}, false, false);
    }
    ApplyEditorPreferencesToAllTabs();
    if (activate_restored_tab) {
      ActivateCurrentTabAfterStateLoad();
    }
    if (log_feedback) {
      LogMessage("Restored workspace session");
    }
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
      if (log_feedback) {
        LogMessage("Opened startup file: " + candidate.filename().string());
      }
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
  return true;
}

void WorkspaceShell::StoreCurrentProjectState(ProjectWorkspaceState& state) {
  SyncActiveEditorTab();
  StopProjectSearch();
  CloseTreeContextMenu();

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
  state.sidebar_visible = sidebar_visible_;
  state.sidebar_mode = sidebar_mode_;
  state.sidebar_prev_mode = sidebar_prev_mode_;
  state.sidebar_temporary = sidebar_temporary_;
  state.overlay_visible = overlay_visible_;
  state.overlay_mode = overlay_mode_;
  state.buffer_search_field = buffer_search_field_;
  state.command_mode = command_mode_;
  state.focus = focus_;
  state.sidebar_width = sidebar_width_;
  state.bottom_panel_height = bottom_panel_height_;
  state.sidebar_scroll_row = sidebar_scroll_row_;
  state.overlay_scroll_row = overlay_scroll_row_;
  state.terminal_tabs = std::move(terminal_tabs_);
  state.active_terminal_tab_index = active_terminal_tab_index_;
  state.buffer_search_query = std::move(buffer_search_query_);
  state.buffer_replace_text = std::move(buffer_replace_text_);
  state.buffer_search_matches = std::move(buffer_search_matches_);
  state.buffer_search_selected_index = buffer_search_selected_index_;
  state.project_search_query = std::move(project_search_query_);
  state.project_search_options = project_search_options_;
  state.project_search_edit_buffer = std::move(project_search_edit_buffer_);
  state.project_search_editing = project_search_editing_;
  state.project_search_edit_field = project_search_edit_field_;
  state.project_replace_text = std::move(project_replace_text_);
  state.project_search_results = std::move(project_search_results_);
  state.project_search_selected_index = project_search_selected_index_;
  state.project_search_running = false;
  state.project_search_truncated = project_search_truncated_;
  state.project_search_error = std::move(project_search_error_);
  state.git_sidebar_entries = std::move(git_sidebar_entries_);
  state.git_base_ref = std::move(git_base_ref_);
  state.git_base_label = std::move(git_base_label_);
  state.git_repo_available = git_repo_available_;
  state.git_sidebar_selected_index = git_sidebar_selected_index_;
  state.compare_picker_path = std::move(compare_picker_path_);
  state.compare_picker_query = std::move(compare_picker_query_);
  state.compare_picker_commits = std::move(compare_picker_commits_);
  state.compare_picker_matches = std::move(compare_picker_matches_);
  state.compare_picker_selected_index = compare_picker_selected_index_;
  state.command_input = std::move(command_input_);
  state.command_history = std::move(command_history_);
  state.command_history_index = command_history_index_;
  state.command_history_pending_input = std::move(command_history_pending_input_);
  state.command_completion_feedback = std::move(command_completion_feedback_);
  state.status_message = std::move(status_message_);
  state.active_colorscheme_name = active_colorscheme_name_;
  state.project_base_color = project_base_color_;
  state.editor_preferences = editor_preferences_;
  RebindProjectState(state);
}

void WorkspaceShell::LoadProjectState(ProjectWorkspaceState& state) {
  StopProjectSearch();
  CloseTreeContextMenu();
  ClearEditorBlame();

  project_root_ = state.root;
  directory_tree_ = std::move(state.directory_tree);
  file_index_ = std::move(state.file_index);
  file_finder_ = std::move(state.file_finder);
  text_viewport_ = std::move(state.text_viewport);
  open_tabs_ = std::move(state.open_tabs);
  active_tab_index_ = state.active_tab_index;
  tab_scroll_index_ = state.tab_scroll_index;
  sidebar_visible_ = state.sidebar_visible;
  sidebar_mode_ = state.sidebar_mode;
  sidebar_prev_mode_ = state.sidebar_prev_mode;
  sidebar_temporary_ = state.sidebar_temporary;
  overlay_visible_ = state.overlay_visible;
  overlay_mode_ = state.overlay_mode;
  buffer_search_field_ = state.buffer_search_field;
  command_mode_ = state.command_mode;
  focus_ = state.focus;
  sidebar_width_ = state.sidebar_width;
  bottom_panel_height_ = state.bottom_panel_height;
  sidebar_scroll_row_ = state.sidebar_scroll_row;
  overlay_scroll_row_ = state.overlay_scroll_row;
  terminal_tabs_ = std::move(state.terminal_tabs);
  active_terminal_tab_index_ = state.active_terminal_tab_index;
  buffer_search_query_ = std::move(state.buffer_search_query);
  buffer_replace_text_ = std::move(state.buffer_replace_text);
  buffer_search_matches_ = std::move(state.buffer_search_matches);
  buffer_search_selected_index_ = state.buffer_search_selected_index;
  project_search_query_ = std::move(state.project_search_query);
  project_search_options_ = state.project_search_options;
  project_search_edit_buffer_ = std::move(state.project_search_edit_buffer);
  project_search_editing_ = state.project_search_editing;
  project_search_edit_field_ = state.project_search_edit_field;
  project_replace_text_ = std::move(state.project_replace_text);
  project_search_results_ = std::move(state.project_search_results);
  project_search_selected_index_ = state.project_search_selected_index;
  project_search_running_ = false;
  project_search_truncated_ = state.project_search_truncated;
  project_search_error_ = std::move(state.project_search_error);
  git_sidebar_entries_ = std::move(state.git_sidebar_entries);
  git_base_ref_ = std::move(state.git_base_ref);
  git_base_label_ = std::move(state.git_base_label);
  git_repo_available_ = state.git_repo_available;
  git_sidebar_selected_index_ = state.git_sidebar_selected_index;
  project_search_run_id_ = 0;
  compare_picker_path_ = std::move(state.compare_picker_path);
  compare_picker_query_ = std::move(state.compare_picker_query);
  compare_picker_commits_ = std::move(state.compare_picker_commits);
  compare_picker_matches_ = std::move(state.compare_picker_matches);
  compare_picker_selected_index_ = state.compare_picker_selected_index;
  command_input_ = std::move(state.command_input);
  command_history_ = std::move(state.command_history);
  command_history_index_ = state.command_history_index;
  command_history_pending_input_ = std::move(state.command_history_pending_input);
  command_completion_feedback_ = std::move(state.command_completion_feedback);
  status_message_ = std::move(state.status_message);
  active_colorscheme_name_ = state.active_colorscheme_name;
  project_base_color_ = state.project_base_color;
  editor_preferences_ = state.editor_preferences;

  state.root = project_root_;
  file_finder_.SetIndex(&file_index_);
  ApplyColorscheme(active_colorscheme_name_, false, false);
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
  sidebar_scroll_row_ = 0;
  RefreshGitSidebar();

  if (sidebar_mode_ == SidebarMode::Search && !project_search_query_.empty()) {
    RefreshProjectSearch();
  }
  return true;
}

bool WorkspaceShell::OpenProjectTab(const std::filesystem::path& project_root,
                                    bool restore_persistence,
                                    bool log_feedback) {
  const std::filesystem::path normalized_root = ResolveProjectRootInput(project_root);
  if (normalized_root.empty()) {
    if (log_feedback) {
      LogMessage("Failed to open project: " + project_root.lexically_normal().string());
    }
    return false;
  }

  if (!project_root_.empty() && normalized_root == project_root_) {
    EnsureActiveProjectVisible();
    return true;
  }

  for (std::size_t i = 0; i < projects_.size(); ++i) {
    const std::filesystem::path open_root =
        (!project_root_.empty() && i == active_project_index_) ? project_root_
        : projects_[i] != nullptr                               ? projects_[i]->root
                                                               : std::filesystem::path{};
    if (open_root == normalized_root) {
      return SwitchProject(i, log_feedback);
    }
  }

  const bool had_active_project = !project_root_.empty() && active_project_index_ < projects_.size();
  const std::size_t previous_active_index = active_project_index_;
  if (had_active_project) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  auto project_state = std::make_unique<ProjectWorkspaceState>();
  project_state->root = normalized_root;
  project_state->initialized = true;
  projects_.push_back(std::move(project_state));
  active_project_index_ = projects_.size() - 1;

  if (!InitializeCurrentProject(normalized_root, restore_persistence, log_feedback)) {
    projects_.pop_back();
    if (had_active_project && previous_active_index < projects_.size()) {
      active_project_index_ = previous_active_index;
      ActivateProjectState(*projects_[active_project_index_], true);
    } else {
      active_project_index_ = 0;
      ResetProjectScopedState(true);
    }
    if (log_feedback) {
      LogMessage("Failed to open project: " + normalized_root.string());
    }
    return false;
  }

  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  return true;
}

bool WorkspaceShell::SwitchProject(std::size_t index, bool log_feedback) {
  if (index >= projects_.size()) {
    return false;
  }
  CloseTreeContextMenu();
  if (!project_root_.empty() && index == active_project_index_) {
    EnsureActiveProjectVisible();
    return true;
  }

  if (!project_root_.empty() && active_project_index_ < projects_.size()) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  const std::size_t previous_active_index = active_project_index_;
  active_project_index_ = index;
  if (!ActivateProjectState(*projects_[index], true)) {
    if (!project_root_.empty() && previous_active_index < projects_.size()) {
      active_project_index_ = previous_active_index;
      ActivateProjectState(*projects_[active_project_index_], true);
    } else {
      active_project_index_ = 0;
      ResetProjectScopedState(true);
    }
    return false;
  }
  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  if (log_feedback) {
    LogMessage("Project switched: " + ProjectLabel());
  }
  return true;
}

bool WorkspaceShell::MoveActiveProjectTo(std::size_t index) {
  if (active_project_index_ >= projects_.size() || index >= projects_.size()) {
    return false;
  }
  if (active_project_index_ == index) {
    return true;
  }

  std::unique_ptr<ProjectWorkspaceState> moved_project =
      std::move(projects_[active_project_index_]);
  projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(active_project_index_));
  projects_.insert(projects_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved_project));
  active_project_index_ = index;
  EnsureActiveProjectVisible();
  return true;
}

void WorkspaceShell::RequestCloseProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }
  if (!DirtyEditorTabIndicesForProject(index).empty()) {
    ShowDirtyPromptForProject(index);
    return;
  }
  CloseProject(index);
}

void WorkspaceShell::CloseProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }

  const bool closing_active = !project_root_.empty() && index == active_project_index_;
  const std::filesystem::path project_root =
      closing_active ? project_root_
                     : (projects_[index] != nullptr ? projects_[index]->root
                                                    : std::filesystem::path{});
  const std::string closed_label = ProjectLabelForRoot(project_root);

  if (closing_active) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[index]);
  }

  projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(index));
  if (projects_.empty()) {
    active_project_index_ = 0;
    project_tab_scroll_index_ = 0;
    ResetProjectScopedState(true);
    SaveWorkspaceSession();
    LogMessage("Closed project: " + closed_label);
    return;
  }

  if (closing_active) {
    active_project_index_ = std::min(index, projects_.size() - 1);
    if (!ActivateProjectState(*projects_[active_project_index_], true)) {
      projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(active_project_index_));
      if (projects_.empty()) {
        active_project_index_ = 0;
        project_tab_scroll_index_ = 0;
        ResetProjectScopedState(true);
        SaveWorkspaceSession();
        LogMessage("Closed project: " + closed_label);
        return;
      }
      active_project_index_ = std::min(active_project_index_, projects_.size() - 1);
      ActivateProjectState(*projects_[active_project_index_], true);
    }
  } else if (active_project_index_ > index) {
    --active_project_index_;
  }

  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  LogMessage("Closed project: " + closed_label);
}

std::filesystem::path WorkspaceShell::ConfigStatePath() const {
  return project_root_.empty() ? std::filesystem::path{} : ProjectStateDirectory() / "config";
}

std::filesystem::path WorkspaceShell::UserConfigPath() const {
  if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
      xdg_config_home != nullptr && *xdg_config_home != '\0') {
    return std::filesystem::path(xdg_config_home) / "microide" / "config";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".config" / "microide" / "config";
  }
  return {};
}

std::filesystem::path WorkspaceShell::ProjectStateDirectory() const {
  if (project_root_.empty()) {
    return {};
  }
  const std::string directory_name = ProjectStateDirectoryName(project_root_);
  if (const char* xdg_state_home = std::getenv("XDG_STATE_HOME");
      xdg_state_home != nullptr && *xdg_state_home != '\0') {
    return std::filesystem::path(xdg_state_home) / "microide" / "projects" / directory_name;
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".local" / "state" / "microide" / "projects" /
           directory_name;
  }
  return {};
}

}  // namespace microide::workspace
