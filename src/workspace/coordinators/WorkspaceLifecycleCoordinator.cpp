#include <memory>

#include "workspace/coordinators/WorkspaceLifecycleCoordinator.h"
#include "workspace/services/TerminalPanelService.h"

#include <optional>
#include <system_error>

#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

#include "app/SdlWaker.h"
#include "util/BackgroundTaskCounter.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "util/Waker.h"
#include "util/StartupTrace.h"
#include "workspace/SettingFlags.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/coordinators/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ProjectInFront.h"
#include "workspace/shell/WorkspaceShell.h"
#include "util/InvokeIfSet.h"

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
  util::InvokeIfSet(operations_.apply_startup_setting_overrides);
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
  const bool restore_persistence = operations_.restore_project_persistence
                                       ? operations_.restore_project_persistence()
                                       : !operations_.skip_workspace_session_restore();
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
  util::InvokeIfSet(operations_.await_host_terminal_opens);
  operations_.release_host_terminals();
  operations_.persist_active_project();
  operations_.record_inactive_host_terminals();
  operations_.save_workspace_session();
  operations_.flush_recents();
  operations_.flush_persisted_state();
  operations_.persist_remote_mirrors();
  operations_.shutdown_project_search_runtime();
  operations_.stop_control_channel();
  operations_.stop_debug_sessions();
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
  bool any_registration_failed = false;
  const auto register_wake = [&](const char* name) -> util::WakeChannel {
    const util::WakeChannel channel = app::RegisterSdlWakeChannel(name);
    if (channel == 0) {
      any_registration_failed = true;
    }
    return channel;
  };

  util::SetBackgroundTaskWakeChannel(register_wake("background-task"));

  plugin_runtime_.SetWakeChannel(register_wake("plugin-runtime"));

  git_blame_event_type_ = register_wake("git-blame");
  git_blame_service_.SetWakeChannel(git_blame_event_type_);

  git_sidebar_event_type_ = register_wake("git-sidebar");
  commit_workflow_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  git_operation_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  compare_picker_mailbox_.SetWakeChannel(git_sidebar_event_type_);
  InitializeCommitWorkflowService();
  patch_apply_service_.SetCompletionWakeEvent(git_sidebar_event_type_);
  editor_blame_overlay_service_.SetProjectLauncherSource(
      [this]() { return &context_.current_project_state.launcher(); });
  patch_apply_service_.SetCallbacks(PatchApplyService::Callbacks{
      .current_repository_state = [this]() {
        return git_repository_service_.CurrentState();
      },
      .project_launcher = [this]() { return &context_.current_project_state.launcher(); },
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
      .front_project_root = [this]() { return context_.current_project_state.root; },
      .is_in_front = ProjectInFront{context_},
      .project_label = ProjectLabelOf{context_},
      .defer_to_project = CatchUpOnActivate{context_},
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
          [this]() { return util::PushWake(git_sidebar_event_type_); },
  });

  terminal_event_type_ = register_wake("terminal");

  project_file_event_type_ = register_wake("project-files");
  save_formatter_service_.SetWakeChannel(project_file_event_type_);
  file_read_service_.SetWakeChannel(project_file_event_type_);
  file_index_refresh_mailbox_.SetWakeChannel(project_file_event_type_);
  project_replace_mailbox_.SetWakeChannel(project_file_event_type_);
  project_open_dialog_event_type_ = register_wake("open-dialog");

  lsp_event_type_ = register_wake("lsp");
  lsp_service_.SetWakeChannel(lsp_event_type_);
  lsp_service_.EnsureProjectLspManager(context_.current_project_state).SetWakeChannel(lsp_event_type_);
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry != nullptr) {
      lsp_service_.EnsureProjectLspManager(*entry).SetWakeChannel(lsp_event_type_);
    }
  }

  dap_event_type_ = register_wake("dap");
  debug_service_.SetWakeChannel(dap_event_type_);
  EnsureProjectDapManager(context_.current_project_state).SetWakeChannel(dap_event_type_);
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry != nullptr) {
      EnsureProjectDapManager(*entry).SetWakeChannel(dap_event_type_);
    }
  }

  plugin_thread_event_type_ = register_wake("plugin-thread");
  plugin_runtime_.SetPluginThreadEventType(plugin_thread_event_type_);
  plugin_runtime_.Host().SetWorker(&plugin_runtime_.Thread());

  surface_texture_cache_.SetWakeChannel(register_wake("surface-textures"));

  highlight_prefetch_event_type_ = register_wake("highlight-prefetch");
  highlight_prefetch_service_.SetWakeCallback([event_type = highlight_prefetch_event_type_]() {
    util::PushWake(event_type);
  });

  control_event_type_ = register_wake("control");
  control_channel_service_.SetWakeChannel(control_event_type_);
  MaybeStartControlChannel();

  util::SetWakeRegistrationDegraded(any_registration_failed);
}

LifecycleCoordinator& WorkspaceShell::MakeLifecycleCoordinator() {
  if (glue_->lifecycle_coordinator != nullptr) {
    return *glue_->lifecycle_coordinator;
  }
  glue_->lifecycle_coordinator = std::make_unique<LifecycleCoordinator>(
      context_,
      quit_requested_,
      LifecycleCoordinator::Operations{
          .reset_startup_state = [this]() { ResetLifecycleStartupState(); },
          .initialize_project_search_runtime = [this]() { project_search_runtime_.Initialize(); },
          .register_wake_events = [this]() { RegisterLifecycleWakeEvents(); },
          .restore_user_config = [this]() { MakePersistenceCoordinator().RestoreUserConfig(); },
          .apply_startup_setting_overrides = [this]() { ApplyStartupSettingOverrides(); },
          .refresh_available_colorscheme_names =
              [this]() { MakePersistenceCoordinator().RefreshAvailableColorschemeNames(); },
          .reset_project_scoped_state = [this](bool show_welcome) {
            ResetProjectScopedState(show_welcome);
          },
          .skip_workspace_session_restore =
              [this]() {
                return startup_options_.skip_workspace_session_restore() ||
                       !SettingFlagEnabled(GetSettingValue("session.restore_on_launch"), true);
              },
          .restore_project_persistence =
              [this]() {
                return !startup_options_.safe_mode &&
                       SettingFlagEnabled(GetSettingValue("session.restore_on_launch"), true);
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
          .await_host_terminal_opens =
              [this]() {
                const auto in_flight = [this]() {
                  if (TerminalPanelService::HasHostOpenInFlight(context_.current_project_state)) {
                    return true;
                  }
                  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
                    const ProjectWorkspaceState* state = context_.project_catalog.entries[i].get();
                    if (i != context_.project_catalog.active_index && state != nullptr &&
                        TerminalPanelService::HasHostOpenInFlight(*state)) {
                      return true;
                    }
                  }
                  return false;
                };
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (in_flight() && std::chrono::steady_clock::now() < deadline) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
              },
          .release_host_terminals =
              [this]() {
                TerminalPanelService& panel = MakeTerminalPanelService();
                panel.ReleaseHostTerminals(context_.current_project_state);
                for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
                  if (i != context_.project_catalog.active_index &&
                      context_.project_catalog.entries[i] != nullptr) {
                    panel.ReleaseHostTerminals(*context_.project_catalog.entries[i]);
                  }
                }
              },
          .record_inactive_host_terminals =
              [this]() {
                for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
                  ProjectWorkspaceState* state = context_.project_catalog.entries[i].get();
                  if (i == context_.project_catalog.active_index || state == nullptr ||
                      !state->host_terminals_unrecorded) {
                    continue;
                  }
                  if (MakePersistenceCoordinator().SaveTerminalLayout(
                          state->root, TerminalPanelService::CaptureLayout(*state))) {
                    state->host_terminals_unrecorded = false;
                  }
                }
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
          .persist_remote_mirrors = [this]() { MakeRemoteHostService().PersistMirrors(); },
          .shutdown_project_search_runtime = [this]() { project_search_runtime_.Shutdown(); },
          .stop_control_channel = [this]() { control_channel_service_.Stop(); },
          .stop_debug_sessions =
              [this]() {
                debug_service_.StopAllDebugging();
                for (const auto& entry : context_.project_catalog.entries) {
                  if (entry != nullptr && entry->dap_manager != nullptr) {
                    entry->dap_manager->ShutdownAll();
                  }
                }
              },
      });
  return *glue_->lifecycle_coordinator;
}

bool WorkspaceShell::Initialize(const std::filesystem::path& project_root) {
  return MakeLifecycleCoordinator().Initialize(project_root);
}

void WorkspaceShell::Shutdown() {
  file_read_service_.CancelAllAndFlush();
  MakeLifecycleCoordinator().Shutdown();
}

void WorkspaceShell::RequestQuit(bool forced) {
  SettleTabIoBeforeTeardown(save_formatter_service_, file_read_service_);
  if (forced && context_.prompts.dirty_visible) {
    DismissDirtyPrompt(false);
  }
  MakeLifecycleCoordinator().RequestQuit();
}

bool WorkspaceShell::ConsumeQuitRequested() {
  return MakeLifecycleCoordinator().ConsumeQuitRequested();
}

}
