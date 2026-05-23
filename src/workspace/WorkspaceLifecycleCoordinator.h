#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class LifecycleCoordinator {
 public:
  struct Operations {
    std::function<void()> reset_startup_state;
    std::function<void()> initialize_project_search_runtime;
    std::function<void()> register_wake_events;
    std::function<void()> restore_user_config;
    std::function<void()> refresh_available_colorscheme_names;
    std::function<void(bool)> reset_project_scoped_state;
    std::function<void(bool)> set_project_watcher_deferred_arming;
    std::function<bool()> skip_workspace_session_restore;
    std::function<bool()> restore_workspace_session;
    std::function<void()> reload_plugins_for_current_project;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void()> shutdown_plugin_runtime;
    std::function<void()> save_user_config;
    std::function<void()> stop_git_blame_service;
    std::function<void()> persist_active_project;
    std::function<void()> persist_inactive_projects_for_shutdown;
    std::function<void()> save_workspace_session;
    std::function<void()> shutdown_project_search_runtime;
    std::function<void()> clear_terminal_tabs;
    std::function<void()> destroy_cursors;
  };

  LifecycleCoordinator(WorkspaceContext& context, bool& quit_requested, Operations operations);

  bool Initialize(const std::filesystem::path& project_root);
  void Shutdown();
  void RequestQuit();
  bool ConsumeQuitRequested();

 private:
  WorkspaceContext& context_;
  bool& quit_requested_;
  Operations operations_;
};

}  // namespace microide::workspace
