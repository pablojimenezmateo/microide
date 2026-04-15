#include "workspace/WorkspaceLifecycleCoordinator.h"

#include <filesystem>

#include "util/StartupTrace.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"

namespace microide::workspace {

WorkspaceShell::LifecycleCoordinator::LifecycleCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceShell::LifecycleCoordinator::ResetStartupState() {
  shell_.caret_blink_epoch_ms_ = SDL_GetTicks();
  shell_.cursor_kind_ = CursorKind::Default;
  shell_.last_mouse_position_valid_ = false;
  shell_.quit_requested_ = false;
  shell_.prompts_.dirty_visible = false;
  shell_.project_catalog_.entries.clear();
  shell_.project_catalog_.active_index = 0;
  shell_.project_catalog_.tab_scroll_index = 0;
}

void WorkspaceShell::LifecycleCoordinator::RegisterWakeEvents() {
  shell_.git_blame_event_type_ = SDL_RegisterEvents(1);
  if (shell_.git_blame_event_type_ != static_cast<Uint32>(-1)) {
    shell_.git_blame_service_.SetWakeEventType(shell_.git_blame_event_type_);
  } else {
    shell_.git_blame_event_type_ = 0;
  }

  shell_.terminal_event_type_ = SDL_RegisterEvents(1);
  if (shell_.terminal_event_type_ == static_cast<Uint32>(-1)) {
    shell_.terminal_event_type_ = 0;
  }

  shell_.project_open_dialog_event_type_ = SDL_RegisterEvents(1);
  if (shell_.project_open_dialog_event_type_ == static_cast<Uint32>(-1)) {
    shell_.project_open_dialog_event_type_ = 0;
  }
}

void WorkspaceShell::LifecycleCoordinator::DestroyCursors() {
  if (SDL_Cursor* default_cursor = SDL_GetDefaultCursor(); default_cursor != nullptr) {
    SDL_SetCursor(default_cursor);
  }

  if (shell_.text_cursor_ != nullptr) {
    SDL_DestroyCursor(shell_.text_cursor_);
    shell_.text_cursor_ = nullptr;
  }
  if (shell_.pointer_cursor_ != nullptr) {
    SDL_DestroyCursor(shell_.pointer_cursor_);
    shell_.pointer_cursor_ = nullptr;
  }
  if (shell_.ew_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(shell_.ew_resize_cursor_);
    shell_.ew_resize_cursor_ = nullptr;
  }
  if (shell_.ns_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(shell_.ns_resize_cursor_);
    shell_.ns_resize_cursor_ = nullptr;
  }

  shell_.cursor_kind_ = CursorKind::Default;
  shell_.last_mouse_position_valid_ = false;
}

std::size_t WorkspaceShell::LifecycleCoordinator::DirtyProjectTabCount() const {
  std::size_t dirty_count = shell_.DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < shell_.project_catalog_.entries.size(); ++i) {
    if (shell_.HasActiveProjectCatalogEntry() && i == shell_.project_catalog_.active_index) {
      continue;
    }
    dirty_count += shell_.DirtyEditorTabIndicesForProject(i).size();
  }
  return dirty_count;
}

bool WorkspaceShell::LifecycleCoordinator::Initialize(
    const std::filesystem::path& project_root) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::Initialize");
  ResetStartupState();

  shell_.project_search_runtime_.Initialize();
  RegisterWakeEvents();

  {
    util::StartupTrace::Scope restore_config_scope("WorkspaceShell::RestoreUserConfig");
    shell_.RestoreUserConfig();
  }
  {
    util::StartupTrace::Scope refresh_colors_scope(
        "WorkspaceShell::RefreshAvailableColorschemeNames");
    shell_.RefreshAvailableColorschemeNames();
  }
  {
    util::StartupTrace::Scope reset_state_scope("WorkspaceShell::ResetProjectScopedState");
    shell_.ResetProjectScopedState(true);
  }

  {
    util::StartupTrace::Scope restore_workspace_scope("WorkspaceShell::RestoreWorkspaceSession");
    if (shell_.RestoreWorkspaceSession()) {
      return true;
    }
  }

  if (project_root.empty()) {
    shell_.ReloadPluginsForCurrentProject();
    return true;
  }

  util::StartupTrace::Scope open_project_scope("WorkspaceShell::OpenProjectTab");
  return shell_.OpenProjectTab(project_root, true, true);
}

void WorkspaceShell::LifecycleCoordinator::Shutdown() {
  shell_.plugin_host_.Shutdown();
  shell_.SaveUserConfig();
  shell_.git_blame_service_.Stop();

  if (shell_.HasActiveProjectCatalogEntry()) {
    ProjectCatalogCoordinator(shell_).PersistActiveEntry();
  }
  ProjectCatalogCoordinator(shell_).PersistInactiveEntriesForShutdown();
  shell_.SaveWorkspaceSession();

  shell_.project_search_runtime_.Shutdown();
  shell_.terminal_tabs_.clear();
  DestroyCursors();
}

void WorkspaceShell::LifecycleCoordinator::RequestQuit() {
  if (shell_.prompts_.dirty_visible) {
    shell_.surface_.focus = FocusTarget::Overlay;
    return;
  }

  if (DirtyProjectTabCount() == 0) {
    shell_.quit_requested_ = true;
    return;
  }

  shell_.ShowDirtyPromptForQuit();
}

bool WorkspaceShell::LifecycleCoordinator::ConsumeQuitRequested() {
  const bool requested = shell_.quit_requested_;
  shell_.quit_requested_ = false;
  return requested;
}

bool WorkspaceShell::Initialize(const std::filesystem::path& project_root) {
  return LifecycleCoordinator(*this).Initialize(project_root);
}

void WorkspaceShell::Shutdown() {
  LifecycleCoordinator(*this).Shutdown();
}

void WorkspaceShell::RequestQuit() {
  LifecycleCoordinator(*this).RequestQuit();
}

bool WorkspaceShell::ConsumeQuitRequested() {
  return LifecycleCoordinator(*this).ConsumeQuitRequested();
}

}  // namespace microide::workspace
