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
    std::function<bool()> skip_workspace_session_restore;
    std::function<bool()> restore_workspace_session;
    std::function<void()> reload_plugins_for_current_project;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void()> shutdown_plugin_runtime;
    std::function<void()> save_user_config;
    std::function<void()> stop_git_blame_service;
    std::function<void()> persist_active_project;
    std::function<void()> save_workspace_session;
    // The recents MRU coalesces its durable writes, so a burst of opens can leave
    // one pending. The process leaves via quick_exit(), so shutdown is the only
    // point that can land it.
    std::function<void()> flush_recents;
    // Persisted workspace state (session, config, project state) is written on a
    // background worker so a save never stalls the shell. Same reason as
    // flush_recents: the process leaves via quick_exit(), which runs no
    // destructors, so shutdown is the only point that can land what is queued.
    std::function<void()> flush_persisted_state;
    std::function<void()> shutdown_project_search_runtime;
    std::function<void()> stop_control_channel;
    // There is deliberately no clear_terminal_tabs / destroy_cursors here: see
    // the note at the end of Shutdown(). Both existed as wired-but-never-called
    // fields for long enough to read as steps the coordinator performs.
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
