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
    // The `--set` overrides, applied over the user config before any project opens.
    std::function<void()> apply_startup_setting_overrides;
    std::function<void()> refresh_available_colorscheme_names;
    std::function<void(bool)> reset_project_scoped_state;
    std::function<bool()> skip_workspace_session_restore;
    // Whether the project opened at launch restores its own saved state (its
    // config and its session: tabs, splits, carets). Off in safe mode and with
    // session.restore_on_launch off -- but NOT for a project named on the command
    // line, which only replaces the multi-project workspace session.
    std::function<bool()> restore_project_persistence;
    std::function<bool()> restore_workspace_session;
    std::function<void()> reload_plugins_for_current_project;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    // A path given at startup that names a FILE opens that file inside its
    // directory (`microide notes.txt`, as `code notes.txt` does); this is the
    // second half of that, after open_project_tab has opened the directory.
    std::function<void(const std::filesystem::path&)> open_file;
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
    // End every debug session, each with its launched debuggee. quick_exit() does
    // not end them: the adapter only sees its pipes close, gdb then exits, and
    // whether the program it ran goes with it is a race (2 runs in 5 left it
    // running, QA). A disconnect that asks for the debuggee's end does it.
    std::function<void()> stop_debug_sessions;
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
