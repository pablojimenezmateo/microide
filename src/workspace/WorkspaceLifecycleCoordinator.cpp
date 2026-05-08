#include "workspace/WorkspaceLifecycleCoordinator.h"

#include <filesystem>
#include <utility>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/ProjectCatalogService.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

LifecycleCoordinator::LifecycleCoordinator(WorkspaceContext& context,
                                           bool& quit_requested,
                                           Operations operations)
    : context_(context),
      quit_requested_(quit_requested),
      operations_(std::move(operations)) {}

bool LifecycleCoordinator::Initialize(const std::filesystem::path& project_root) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::Initialize");
  operations_.reset_startup_state();
  operations_.set_project_watcher_deferred_arming(true);

  operations_.initialize_project_search_runtime();
  operations_.initialize_ai_provider_runtime();
  operations_.register_wake_events();
  editor::runtime_syntax::EnsureInitialized();

  {
    util::StartupTrace::Scope restore_config_scope("WorkspaceShell::RestoreUserConfig");
    operations_.restore_user_config();
  }
  {
    util::StartupTrace::Scope refresh_colors_scope(
        "WorkspaceShell::RefreshAvailableColorschemeNames");
    operations_.refresh_available_colorscheme_names();
  }
  {
    util::StartupTrace::Scope reset_state_scope("WorkspaceShell::ResetProjectScopedState");
    operations_.reset_project_scoped_state(true);
  }

  {
    util::StartupTrace::Scope restore_workspace_scope("WorkspaceShell::RestoreWorkspaceSession");
    if (operations_.restore_workspace_session()) {
      operations_.set_project_watcher_deferred_arming(false);
      return true;
    }
  }

  if (project_root.empty()) {
    operations_.set_project_watcher_deferred_arming(false);
    operations_.reload_plugins_for_current_project();
    return true;
  }

  util::StartupTrace::Scope open_project_scope("WorkspaceShell::OpenProjectTab");
  const bool opened = operations_.open_project_tab(project_root, true, true);
  operations_.set_project_watcher_deferred_arming(false);
  return opened;
}

void LifecycleCoordinator::Shutdown() {
  operations_.shutdown_plugin_runtime();
  operations_.save_user_config();
  operations_.stop_git_blame_service();
  operations_.persist_active_project();
  operations_.persist_inactive_projects_for_shutdown();
  operations_.save_workspace_session();
  operations_.shutdown_project_search_runtime();
  operations_.shutdown_ai_provider_runtime();
  // Terminal session teardown and cursor cleanup are intentionally skipped here:
  // the process exits via quick_exit() immediately after, so the OS reclaims
  // all child processes and resources without per-terminal blocking waits.
}

void LifecycleCoordinator::RequestQuit() {
  if (context_.prompts.dirty_visible) {
    context_.current_project_state.surface.focus = FocusTarget::Overlay;
    return;
  }
  quit_requested_ = true;
}

bool LifecycleCoordinator::ConsumeQuitRequested() {
  const bool requested = quit_requested_;
  quit_requested_ = false;
  return requested;
}

void WorkspaceShell::ResetLifecycleStartupState() {
  caret_blink_epoch_ms_ = SDL_GetTicks();
  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
  quit_requested_ = false;
  context_.prompts.dirty_visible = false;
  context_.project_catalog.entries.clear();
  context_.project_catalog.active_index = 0;
  context_.project_catalog.tab_scroll_index = 0;
}

void WorkspaceShell::RegisterLifecycleWakeEvents() {
  const Uint32 plugin_asset_event_type = SDL_RegisterEvents(1);
  plugin_runtime_.SetWakeEventType(
      plugin_asset_event_type != static_cast<Uint32>(-1) ? plugin_asset_event_type : 0);

  git_blame_event_type_ = SDL_RegisterEvents(1);
  if (git_blame_event_type_ != static_cast<Uint32>(-1)) {
    git_blame_service_.SetWakeEventType(git_blame_event_type_);
  } else {
    git_blame_event_type_ = 0;
  }

  git_sidebar_event_type_ = SDL_RegisterEvents(1);
  if (git_sidebar_event_type_ == static_cast<Uint32>(-1)) {
    git_sidebar_event_type_ = 0;
  }

  terminal_event_type_ = SDL_RegisterEvents(1);
  if (terminal_event_type_ == static_cast<Uint32>(-1)) {
    terminal_event_type_ = 0;
  }

  project_file_event_type_ = SDL_RegisterEvents(1);
  project_file_monitor_.SetWakeEventType(
      project_file_event_type_ != static_cast<Uint32>(-1) ? project_file_event_type_ : 0);
  if (project_file_event_type_ == static_cast<Uint32>(-1)) {
    project_file_event_type_ = 0;
  }

  project_open_dialog_event_type_ = SDL_RegisterEvents(1);
  if (project_open_dialog_event_type_ == static_cast<Uint32>(-1)) {
    project_open_dialog_event_type_ = 0;
  }

  lsp_event_type_ = SDL_RegisterEvents(1);
  if (lsp_event_type_ != static_cast<Uint32>(-1)) {
    EnsureProjectLspManager(context_.current_project_state).SetWakeEventType(lsp_event_type_);
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr) {
        EnsureProjectLspManager(*entry).SetWakeEventType(lsp_event_type_);
      }
    }
  } else {
    lsp_event_type_ = 0;
  }

  plugin_async_process_event_type_ = SDL_RegisterEvents(1);
  if (plugin_async_process_event_type_ != static_cast<Uint32>(-1)) {
    plugin_runtime_.SetAsyncProcessEventType(plugin_async_process_event_type_);
  } else {
    plugin_async_process_event_type_ = 0;
  }
}

void WorkspaceShell::DestroyLifecycleCursors() {
  if (SDL_Cursor* default_cursor = SDL_GetDefaultCursor(); default_cursor != nullptr) {
    SDL_SetCursor(default_cursor);
  }

  if (text_cursor_ != nullptr) {
    SDL_DestroyCursor(text_cursor_);
    text_cursor_ = nullptr;
  }
  if (pointer_cursor_ != nullptr) {
    SDL_DestroyCursor(pointer_cursor_);
    pointer_cursor_ = nullptr;
  }
  if (ew_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ew_resize_cursor_);
    ew_resize_cursor_ = nullptr;
  }
  if (ns_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ns_resize_cursor_);
    ns_resize_cursor_ = nullptr;
  }

  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
}

LifecycleCoordinator WorkspaceShell::MakeLifecycleCoordinator() {
  return LifecycleCoordinator(
      context_,
      quit_requested_,
      LifecycleCoordinator::Operations{
          .reset_startup_state = [this]() { ResetLifecycleStartupState(); },
          .initialize_project_search_runtime = [this]() { project_search_runtime_.Initialize(); },
          .initialize_ai_provider_runtime = [this]() { ai_provider_runtime_service_.Initialize(); },
          .register_wake_events = [this]() { RegisterLifecycleWakeEvents(); },
          .restore_user_config = [this]() { MakePersistenceCoordinator().RestoreUserConfig(); },
          .refresh_available_colorscheme_names =
              [this]() { MakePersistenceCoordinator().RefreshAvailableColorschemeNames(); },
          .reset_project_scoped_state = [this](bool show_welcome) {
            ResetProjectScopedState(show_welcome);
          },
          .set_project_watcher_deferred_arming =
              [this](bool deferred) { project_file_monitor_.SetDeferredArming(deferred); },
          .restore_workspace_session =
              [this]() { return MakePersistenceCoordinator().RestoreWorkspaceSession(); },
          .reload_plugins_for_current_project = [this]() { ReloadPluginsForCurrentProject(); },
          .open_project_tab =
              [this](const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback) {
                return OpenProjectTab(project_root, restore_persistence, log_feedback);
              },
          .shutdown_plugin_runtime = [this]() { plugin_runtime_.Shutdown(); },
          .save_user_config = [this]() { MakePersistenceCoordinator().SaveUserConfig(); },
          .stop_git_blame_service = [this]() { git_blame_service_.Stop(); },
          .persist_active_project =
              [this]() {
                if (HasActiveProjectCatalogEntry()) {
                  MakeProjectCatalogService().PersistActiveEntry();
                }
              },
          .persist_inactive_projects_for_shutdown =
              [this]() { MakeProjectCatalogService().PersistInactiveEntriesForShutdown(); },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .shutdown_project_search_runtime = [this]() { project_search_runtime_.Shutdown(); },
          .shutdown_ai_provider_runtime = [this]() { ai_provider_runtime_service_.Shutdown(); },
          .clear_terminal_tabs =
              [this]() { context_.current_project_state.terminal_tabs.clear(); },
          .destroy_cursors = [this]() { DestroyLifecycleCursors(); },
      });
}

bool WorkspaceShell::Initialize(const std::filesystem::path& project_root) {
  return MakeLifecycleCoordinator().Initialize(project_root);
}

void WorkspaceShell::Shutdown() {
  MakeLifecycleCoordinator().Shutdown();
}

void WorkspaceShell::RequestQuit() {
  MakeLifecycleCoordinator().RequestQuit();
}

bool WorkspaceShell::ConsumeQuitRequested() {
  return MakeLifecycleCoordinator().ConsumeQuitRequested();
}

}  // namespace microide::workspace
