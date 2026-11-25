#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class PersistenceCoordinator;
class WorkspacePluginRuntime;
class RecentsService;

class LifecycleCoordinator {
 public:
  struct Services {
    PersistenceCoordinator& persistence;
    WorkspacePluginRuntime& plugins;
    RecentsService& recents;
  };

  struct Operations {
    std::function<void()> reset_startup_state;
    std::function<void()> initialize_project_search_runtime;
    std::function<void()> register_wake_events;
    std::function<void()> apply_startup_setting_overrides;
    std::function<void(bool)> reset_project_scoped_state;
    std::function<bool()> skip_workspace_session_restore;
    std::function<bool()> restore_project_persistence;
    std::function<void()> reload_plugins_for_current_project;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<void()> stop_git_blame_service;
    std::function<void()> await_host_terminal_opens;
    std::function<void()> release_host_terminals;
    std::function<void()> record_inactive_host_terminals;
    std::function<void()> persist_active_project;
    std::function<void()> flush_persisted_state;
    std::function<void()> persist_remote_mirrors;
    std::function<void()> shutdown_project_search_runtime;
    std::function<void()> stop_control_channel;
    std::function<void()> stop_debug_sessions;
  };

  LifecycleCoordinator(WorkspaceContext& context, bool& quit_requested, Services services, Operations operations);

  bool Initialize(const std::filesystem::path& project_root);
  void Shutdown();
  void RequestQuit();
  bool ConsumeQuitRequested();

 private:
  WorkspaceContext& context_;
  bool& quit_requested_;
  Services services_;
  Operations operations_;
};

}
