#include "workspace/coordinators/WorkspaceLifecycleCoordinator.h"

#include <optional>
#include <system_error>

#include <filesystem>
#include <utility>

#include "app/BackgroundTaskCounter.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "util/SdlWake.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/coordinators/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/shell/WorkspaceShell.h"

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

  {
    util::StartupTrace::Scope project_search_scope(
        "WorkspaceShell::InitializeProjectSearchRuntime");
    operations_.initialize_project_search_runtime();
  }
  {
    util::StartupTrace::Scope register_wake_scope("WorkspaceShell::RegisterWakeEvents");
    operations_.register_wake_events();
  }
  {
    util::StartupTrace::Scope syntax_scope("RuntimeSyntaxRegistry::EnsureInitialized");
    editor::runtime_syntax::EnsureInitialized();
  }

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

  if (!operations_.skip_workspace_session_restore()) {
    util::StartupTrace::Scope restore_workspace_scope("WorkspaceShell::RestoreWorkspaceSession");
    if (operations_.restore_workspace_session()) {
      return true;
    }
  }

  if (project_root.empty()) {
    operations_.reload_plugins_for_current_project();
    return true;
  }

  // `microide notes.txt`: a path that names a regular file is not a project
  // root -- the tree and the index refused it and the whole launch failed with
  // "Workspace initialization failed". Open its directory as the project and
  // the file itself as the first tab, which is what `code notes.txt` does.
  std::filesystem::path root = project_root;
  std::optional<std::filesystem::path> file_to_open;
  {
    std::error_code error;
    if (std::filesystem::is_regular_file(project_root, error) && !error) {
      std::filesystem::path absolute = std::filesystem::absolute(project_root, error);
      if (!error) {
        absolute = absolute.lexically_normal();
        root = absolute.parent_path();
        file_to_open = std::move(absolute);
      }
    }
  }

  util::StartupTrace::Scope open_project_scope("WorkspaceShell::OpenProjectTab");
  const bool restore_persistence = !operations_.skip_workspace_session_restore();
  if (!operations_.open_project_tab(root, restore_persistence, true)) {
    return false;
  }
  if (file_to_open.has_value() && operations_.open_file) {
    operations_.open_file(*file_to_open);
  }
  return true;
}

void LifecycleCoordinator::Shutdown() {
  operations_.shutdown_plugin_runtime();
  operations_.save_user_config();
  operations_.stop_git_blame_service();
  operations_.persist_active_project();
  // Inactive projects are already durably persisted the moment they are switched
  // away from (PersistActiveEntry writes config+session before the state is moved
  // into the catalog entry), and nothing mutates an inactive project while it is
  // inactive. Re-persisting them here would re-hydrate and re-write identical data
  // for every inactive project, so it is intentionally omitted.
  operations_.save_workspace_session();
  operations_.flush_recents();
  // Land every state write the saves above only QUEUED. This has to come after
  // all of them, and it has to be here rather than in a destructor: the process
  // exits via quick_exit() right after Shutdown() returns, so nothing downstream
  // of this point runs.
  operations_.flush_persisted_state();
  operations_.shutdown_project_search_runtime();
  // Stop the control listener and remove the discovery descriptor. Cheap (the
  // I/O thread wakes via its self-pipe, so the join returns immediately) and off
  // the visible path — the window is already destroyed before Shutdown() runs.
  operations_.stop_control_channel();
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
  // Wrap SDL_RegisterEvents so a failed allocation (returns -1) is recorded once. When
  // any channel fails, its worker still runs but has no wake event to drain its ready
  // state; CurrentIdleWaitState then falls back to a bounded poll so pending work is not
  // stranded until unrelated input arrives. (TD-2026-07-16-56.)
  bool any_registration_failed = false;
  const auto register_wake = [&]() -> Uint32 {
    const Uint32 type = SDL_RegisterEvents(1);
    if (type == static_cast<Uint32>(-1)) {
      any_registration_failed = true;
    }
    return type;
  };

  // Dedicated neutral wake for background-task completion. Without a registered
  // type the counter falls back to the bare SDL_EVENT_USER base, which aliases
  // whichever subsystem registered first (project search today) and mis-routes
  // every task-completion wake into that handler. A registered type is claimed by
  // no dispatch branch, so it reaches the neutral default.
  const Uint32 background_task_event_type = register_wake();
  if (background_task_event_type != static_cast<Uint32>(-1)) {
    app::SetBackgroundTaskWakeEventType(background_task_event_type);
  }

  const Uint32 plugin_asset_event_type = register_wake();
  plugin_runtime_.SetWakeEventType(
      plugin_asset_event_type != static_cast<Uint32>(-1) ? plugin_asset_event_type : 0);

  git_blame_event_type_ = register_wake();
  if (git_blame_event_type_ != static_cast<Uint32>(-1)) {
    git_blame_service_.SetWakeEventType(git_blame_event_type_);
  } else {
    git_blame_event_type_ = 0;
  }

  git_sidebar_event_type_ = register_wake();
  if (git_sidebar_event_type_ == static_cast<Uint32>(-1)) {
    git_sidebar_event_type_ = 0;
  }
  // Reuse the git-sidebar wake for commit completions: a commit result is
  // marshaled back to the main thread and drained in ConsumeGitSidebarRefresh
  // (a successful commit refreshes the sidebar anyway).
  commit_workflow_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  // Same wake channel: every git write operation ends in a git refresh anyway.
  git_operation_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  // The async compare/ref picker marshals its git result back through the same
  // wake (drained alongside the sidebar refresh in ConsumeGitSidebarRefresh).
  compare_picker_mailbox_.SetWakeEventType(git_sidebar_event_type_);
  InitializeCommitWorkflowService();
  // The patch apply's post-apply shell work rides the same wake and drain.
  patch_apply_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  patch_apply_service_.SetCallbacks(PatchApplyService::Callbacks{
      .current_repository_state = [this]() {
        return git_repository_service_.CurrentState();
      },
      .request_git_refresh = [this]() { RequestAutomaticGitSidebarRefresh(); },
      .refresh_compare_tab_for_path =
          [this](const std::filesystem::path& path) { RefreshOpenCompareTabsForPath(path); },
      .invalidate_editor_blame_path =
          [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
      .reload_clean_editor_tabs_for_path =
          [this](const std::filesystem::path& path) { ReloadCleanEditorTabsForPath(path); },
      .set_command_feedback =
          [this](std::string_view feedback) {
            context_.current_project_state.panel.feedback.text = std::string(feedback);
          },
      .open_discard_preview_prompt =
          [this](project::PatchApplyPreview preview) {
            OpenPromptSurface(PromptSurfaceState::Action::DiscardPatchPreview,
                              PromptSurfaceState::Kind::Confirm, ActiveCompareTab() != nullptr
                                  ? ActiveCompareTab()->path
                                  : std::filesystem::path{},
                              {});
            context_.prompts.surface.detail = std::move(preview.summary);
            if (!preview.patch_text.empty()) {
              context_.prompts.surface.detail += "\n\n";
              context_.prompts.surface.detail += preview.patch_text;
            }
          },
  });

  git_repository_service_.SetWakeCallbacks(GitRepositoryService::WakeCallbacks{
      .push_refresh_ready_event =
          [this]() { return util::PushSdlWake(git_sidebar_event_type_); },
  });

  terminal_event_type_ = register_wake();
  if (terminal_event_type_ == static_cast<Uint32>(-1)) {
    terminal_event_type_ = 0;
  }

  project_file_event_type_ = register_wake();
  if (project_file_event_type_ == static_cast<Uint32>(-1)) {
    project_file_event_type_ = 0;
  }
  // Off-thread forced-rescan and project-replace-all results wake and drain on the
  // same project-file path.
  file_index_refresh_mailbox_.SetWakeEventType(project_file_event_type_);
  project_replace_mailbox_.SetWakeEventType(project_file_event_type_);
  project_open_dialog_event_type_ = register_wake();
  if (project_open_dialog_event_type_ == static_cast<Uint32>(-1)) {
    project_open_dialog_event_type_ = 0;
  }

  lsp_event_type_ = register_wake();
  if (lsp_event_type_ != static_cast<Uint32>(-1)) {
    lsp_service_.SetWakeEventType(lsp_event_type_);
    EnsureProjectLspManager(context_.current_project_state).SetWakeEventType(lsp_event_type_);
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr) {
        EnsureProjectLspManager(*entry).SetWakeEventType(lsp_event_type_);
      }
    }
  } else {
    lsp_event_type_ = 0;
  }

  dap_event_type_ = register_wake();
  if (dap_event_type_ != static_cast<Uint32>(-1)) {
    debug_service_.SetWakeEventType(dap_event_type_);
    EnsureProjectDapManager(context_.current_project_state).SetWakeEventType(dap_event_type_);
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr) {
        EnsureProjectDapManager(*entry).SetWakeEventType(dap_event_type_);
      }
    }
  } else {
    dap_event_type_ = 0;
  }

  plugin_thread_event_type_ = register_wake();
  if (plugin_thread_event_type_ != static_cast<Uint32>(-1)) {
    plugin_runtime_.SetPluginThreadEventType(plugin_thread_event_type_);
  } else {
    plugin_thread_event_type_ = 0;
  }
  // Hand the host the worker so every plugin Lua call routes off the UI thread.
  // The worker itself stays unspawned until the first Reload that loads a plugin.
  plugin_runtime_.Host().SetWorker(&plugin_runtime_.Thread());

  highlight_prefetch_event_type_ = register_wake();
  if (highlight_prefetch_event_type_ == static_cast<Uint32>(-1)) {
    highlight_prefetch_event_type_ = 0;
  }
  // The worker fires this from its own thread once a result is queued; the wake
  // only nudges the main loop to drain (the event carries no payload).
  highlight_prefetch_service_.SetWakeCallback([event_type = highlight_prefetch_event_type_]() {
    // Checked push: the prefetched highlights are already queued in the service, so a
    // rejected push latches the shared "wake owed" bit for the idle-poll fallback
    // rather than hiding already-computed highlights until unrelated input.
    util::PushSdlWake(event_type);
  });

  // Control channel. Always allocate the wake event + bind it so the marshaling
  // path is ready; only the socket listener is gated on `control.enabled`, which
  // is what lets the channel be toggled on at runtime without a restart.
  control_event_type_ = register_wake();
  if (control_event_type_ == static_cast<Uint32>(-1)) {
    control_event_type_ = 0;
  }
  control_channel_service_.SetWakeEventType(control_event_type_);
  MaybeStartControlChannel();

  // Record whether any wake channel failed to register so the idle-wait falls back to a
  // bounded poll (rather than blocking forever) for the affected subsystems' ready
  // state. (TD-2026-07-16-56.)
  util::SetSdlWakeRegistrationDegraded(any_registration_failed);
}

LifecycleCoordinator WorkspaceShell::MakeLifecycleCoordinator() {
  return LifecycleCoordinator(
      context_,
      quit_requested_,
      LifecycleCoordinator::Operations{
          .reset_startup_state = [this]() { ResetLifecycleStartupState(); },
          .initialize_project_search_runtime = [this]() { project_search_runtime_.Initialize(); },
          .register_wake_events = [this]() { RegisterLifecycleWakeEvents(); },
          .restore_user_config = [this]() { MakePersistenceCoordinator().RestoreUserConfig(); },
          .refresh_available_colorscheme_names =
              [this]() { MakePersistenceCoordinator().RefreshAvailableColorschemeNames(); },
          .reset_project_scoped_state = [this](bool show_welcome) {
            ResetProjectScopedState(show_welcome);
          },
          .skip_workspace_session_restore =
              [this]() {
                // Startup flags (safe mode / --no-restore) or the user setting can
                // each suppress session restore on launch.
                return startup_options_.skip_workspace_session_restore() ||
                       !SettingFlagEnabled(GetSettingValue("session.restore_on_launch"), true);
              },
          .restore_workspace_session =
              [this]() { return MakePersistenceCoordinator().RestoreWorkspaceSession(); },
          .reload_plugins_for_current_project = [this]() { ReloadPluginsForCurrentProject(); },
          .open_project_tab =
              [this](const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback) {
                return OpenProjectTab(project_root, restore_persistence, log_feedback);
              },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .shutdown_plugin_runtime = [this]() { plugin_runtime_.Shutdown(); },
          .save_user_config = [this]() { MakePersistenceCoordinator().SaveUserConfig(); },
          .stop_git_blame_service =
              [this]() {
                git_blame_service_.Stop();
                highlight_prefetch_service_.Shutdown();
              },
          .persist_active_project =
              [this]() {
                if (HasActiveProjectCatalogEntry()) {
                  MakeProjectCatalogService().PersistActiveEntry();
                }
              },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .flush_recents = [this]() { recents_service_.FlushPendingSave(); },
          .flush_persisted_state = [this]() { persistence_service_.FlushPendingWrites(); },
          .shutdown_project_search_runtime = [this]() { project_search_runtime_.Shutdown(); },
          .stop_control_channel = [this]() { control_channel_service_.Stop(); },
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
