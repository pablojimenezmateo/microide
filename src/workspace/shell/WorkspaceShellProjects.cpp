#include <memory>

#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <chrono>

#include "workspace/SettingFlags.h"
#include "workspace/TabReorder.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/services/RemoteHostService.h"

namespace microide::workspace {

ProjectCatalogService& WorkspaceShell::MakeProjectCatalogService() {
  if (glue_->project_catalog_service != nullptr) {
    return *glue_->project_catalog_service;
  }
  glue_->project_catalog_service = std::make_unique<ProjectCatalogService>(
      context_,
      ProjectCatalogService::Operations{
          .initialize_current_project =
              [this](const project::ProjectId& id,
                     const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback,
                     bool activate_restored_tab,
                     const project::ProjectLocality& locality) {
                // A remote project gets its host's locality and (re)connects -- read
                // from its id, fixed when its catalog entry was created, not sniffed
                // from the directory here. An explicit locality always wins (the
                // parity suite's loopback host).
                project::ProjectLocality resolved = locality;
                const bool remote_project =
                    id.IsRemote() && resolved.launcher == &platform::LocalProcessLauncher();
                if (remote_project) {
                  // A remote project is never opened as a local folder: no mirror,
                  // no project (LocalityFor has said why).
                  const auto remote = MakeRemoteHostService().LocalityFor(id);
                  if (!remote.has_value()) {
                    return false;
                  }
                  resolved = *remote;
                }
                const bool initialized = InitializeCurrentProject(
                    id, project_root, restore_persistence, log_feedback, activate_restored_tab, resolved);
                if (initialized && remote_project) {
                  // Reported before this state had its root: what the sync has not
                  // fetched yet is listed, and the connection shown, from the start.
                  RemoteHostService& hosts = MakeRemoteHostService();
                  context_.current_project_state.SetPendingPaths(
                      hosts.PendingPathsOf(context_.current_project_state.root));
                  context_.current_project_state.remote_status =
                      hosts.ProjectStatusOf(context_.current_project_state.root);
                  ++context_.current_project_state.remote_status_revision;
                }
                return initialized;
              },
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .stop_project_search = [this]() { StopProjectSearch(); },
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
          .clear_editor_blame = [this]() { ClearEditorBlame(); },
          .start_file_index_watcher_for_current_project =
              [this]() { return StartFileIndexWatcherForCurrentProject(); },
          .stop_file_index_watcher = [this]() { StopFileIndexWatcher(); },
          .rebind_project_state = [this](ProjectWorkspaceState& state) { RebindProjectState(state); },
          .reset_current_project_state_storage =
              [this]() { ResetCurrentProjectStateStorage(); },
          .reset_transient_interaction_state = [this]() { ResetTransientInteractionState(); },
          .apply_colorscheme =
              [this]() {
                MakePersistenceCoordinator().ApplyColorscheme(
                    context_.current_project_state.active_colorscheme_name, false, false);
              },
          .apply_editor_preferences_to_all_tabs =
              [this]() { ApplyEditorPreferencesToAllTabs(); },
          .apply_welcome_editor_preferences_if_placeholder =
              [this]() {
                if (context_.current_project_state.focused_group().welcome_surface.viewport.is_placeholder()) {
                  ApplyEditorPreferences(context_.current_project_state.focused_group().welcome_surface.viewport);
                }
              },
          .ensure_terminal_tab_open =
              [this]() {
                if (!context_.current_project_state.root.empty() &&
                    context_.current_project_state.terminal_tabs.empty()) {
                  MakeTerminalPanelService().OpenTerminalsForProjectInit();
                }
              },
          .activate_current_tab_after_state_load =
              [this]() { return ActivateCurrentTabAfterStateLoad(); },
          .refresh_plugin_surfaces_for_reactivation =
              [this]() { RefreshPluginSurfacesForReactivation(); },
          .save_config_state = [this]() { MakePersistenceCoordinator().SaveConfigState(); },
          .save_session_state = [this]() { MakePersistenceCoordinator().SaveSessionState(); },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .shutdown_plugin_host = [this]() { plugin_runtime_.ShutdownHost(); },
          .reset_project_catalog_to_welcome_state =
              [this]() { ResetProjectCatalogToWelcomeState(); },
          .ensure_active_project_visible = [this]() { tab_strip_chrome_.EnsureActiveProjectVisible(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
      });
  return *glue_->project_catalog_service;
}

bool WorkspaceShell::HasActiveProjectCatalogEntry() const {
  return context_.HasActiveProjectCatalogEntry();
}

WorkspaceShell::ProjectWorkspaceState* WorkspaceShell::ProjectCatalogEntry(std::size_t index) {
  return context_.ProjectCatalogEntry(index);
}

const WorkspaceShell::ProjectWorkspaceState* WorkspaceShell::ProjectCatalogEntry(
    std::size_t index) const {
  return context_.ProjectCatalogEntry(index);
}

const std::filesystem::path& WorkspaceShell::ProjectCatalogRoot(std::size_t index) const {
  return context_.ProjectCatalogRoot(index);
}

void WorkspaceShell::ResetProjectCatalogToWelcomeState() {
  context_.project_catalog.active_index = 0;
  context_.project_catalog.tab_scroll_index = 0;
  ResetProjectScopedState(true);
  ReloadPluginsForCurrentProject();
  RequestWindowRedraw();
}

bool WorkspaceShell::OpenProjectTab(const std::filesystem::path& project_root,
                                    bool restore_persistence,
                                    bool log_feedback,
                                    const project::ProjectLocality& locality) {
  const std::filesystem::path normalized_root = ResolveProjectRootInput(project_root);
  if (normalized_root.empty()) {
    return false;
  }

  if (!context_.current_project_state.root.empty() && normalized_root == context_.current_project_state.root) {
    tab_strip_chrome_.EnsureActiveProjectVisible();
    return true;
  }

  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (ProjectCatalogRoot(i) == normalized_root) {
      const bool switched = SwitchProject(i, log_feedback);
      if (switched) {
        NoteLayoutInputsChanged();
      }
      return switched;
    }
  }

  // The one place a catalog entry is created from a typed, picked or recent path:
  // the path is named here, once (a mirror's own tree is its remote project).
  const project::ProjectId id = project::ResolveProjectId(normalized_root);
  const bool opened =
      MakeProjectCatalogService().Open(id, normalized_root, restore_persistence, log_feedback,
                                       locality);
  if (opened) {
    recents_service_.RecordProjectOpen(id);
    NoteLayoutInputsChanged();
  }
  return opened;
}

bool WorkspaceShell::SwitchProject(std::size_t index, bool log_feedback) {
  (void) log_feedback;
  SettleTabIoBeforeTeardown(save_formatter_service_, file_read_service_);
  if (index >= context_.project_catalog.entries.size()) {
    return false;
  }
  MakeMenuCoordinator().CloseTreeContextMenu();
  if (HasActiveProjectCatalogEntry() && index == context_.project_catalog.active_index) {
    tab_strip_chrome_.EnsureActiveProjectVisible();
    return true;
  }
  const bool switched = MakeProjectCatalogService().Switch(index);
  if (switched) {
    NoteLayoutInputsChanged();
  }
  return switched;
}

bool WorkspaceShell::MoveActiveProjectTo(std::size_t index) {
  if (!ReorderActive(context_.project_catalog.entries, context_.project_catalog.active_index,
                     index)) {
    return false;
  }
  tab_strip_chrome_.EnsureActiveProjectVisible();
  return true;
}

void WorkspaceShell::RequestCloseProject(std::size_t index) {
  if (index >= context_.project_catalog.entries.size()) {
    return;
  }
  if (HasDirtyEditorTabForProject(index)) {
    ShowDirtyPromptForProject(index);
    return;
  }
  CloseProject(index);
}

void WorkspaceShell::CloseProject(std::size_t index) {
  SettleTabIoBeforeTeardown(save_formatter_service_, file_read_service_);
  const std::filesystem::path closed_root = ProjectCatalogRoot(index);
  // A remote project's running host shells stay on the host (§ 6.12): let go of
  // them, explicitly, before the close persists the state (recording which they
  // are) and destroys it -- a destroyed session would close its shell. With
  // terminal.restore_sessions off nothing would reattach them: end them instead.
  if (ProjectWorkspaceState* state = context_.ProjectStateForRoot(closed_root); state != nullptr) {
    MakeTerminalPanelService().ReleaseHostTerminals(*state);
  }
  MakeProjectCatalogService().Close(index);
  // A remote project's channel, host-side workspace and watch go with its tab; the
  // mirror and its journal stay, and reopening reconnects.
  if (glue_->remote_host_service != nullptr) {
    glue_->remote_host_service->ProjectClosed(closed_root);
  }
  // Closing may drop the open-project count to <= 1, which can re-hide the strip when
  // "chrome.project_tabs.hide_when_single" is on; the coordinator only requests a redraw,
  // so mark layout dirty here to force a recompute (Open/Switch already do this).
  NoteLayoutInputsChanged();
}

bool WorkspaceShell::ProjectTabStripVisible() const {
  if (context_.project_catalog.entries.size() > 1) {
    return true;
  }
  // Resolve the hide-when-single flag at most once per settings-store revision.
  // This runs inside the uncached ComputeLayout on the per-mouse-move window-drag
  // hit-test path, so avoid the string-keyed lookup + default-value allocation on
  // every recompute (mirrors the terminal-font revision gate).
  const std::uint64_t settings_revision = settings_store_.Revision();
  if (settings_revision != project_tabs_hide_when_single_revision_) {
    project_tabs_hide_when_single_revision_ = settings_revision;
    project_tabs_hide_when_single_ =
        SettingFlagEnabled(GetSettingValue("chrome.project_tabs.hide_when_single"),
                           /*default_value=*/true);
  }
  return !project_tabs_hide_when_single_;
}

}  // namespace microide::workspace
