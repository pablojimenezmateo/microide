#include <memory>

#include "workspace/ProjectRootPresence.h"
#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <chrono>

#include "workspace/SettingFlags.h"
#include "workspace/TabReorder.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/services/RemoteHostService.h"

namespace microide::workspace {

ProjectCatalogService& WorkspaceShell::MakeProjectCatalogService() {
  if (glue_->project_catalog_service != nullptr) {
    return *glue_->project_catalog_service;
  }
  glue_->project_catalog_service = std::make_unique<ProjectCatalogService>(
      context_, ProjectCatalogService::Services{.prompts = MakePromptSurfaceService(), .menus = MakeMenuCoordinator()},
      ProjectCatalogService::Operations{
          .initialize_current_project =
              [this](const project::ProjectId& id,
                     const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback,
                     bool activate_restored_tab,
                     bool root_missing,
                     const project::ProjectLocality& locality) {
                project::ProjectLocality resolved = locality;
                const bool remote_project =
                    id.IsRemote() && resolved.launcher == &platform::LocalProcessLauncher();
                if (remote_project) {
                  const auto remote = MakeRemoteHostService().LocalityFor(id);
                  if (!remote.has_value()) {
                    return false;
                  }
                  resolved = *remote;
                } else if (resolved.write_gate == &project::LocalFileWriteGate()) {
                  resolved.write_gate = &MakeRemoteHostService().LocalProjectWriteGate();
                }
                const bool initialized = InitializeCurrentProject(
                    id, project_root, restore_persistence, log_feedback, activate_restored_tab, root_missing, resolved);
                if (initialized && remote_project) {
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
          .catch_up_background_changes =
              [this]() {
                RequestAutomaticGitSidebarRefresh();
                RefreshProjectFiles();
                ReloadCleanOpenBuffersFromDisk();
                std::vector<std::filesystem::path> compared;
                for (const EditorGroup& group : context_.current_project_state.editor_groups) {
                  for (const TabEntry& tab : group.open_tabs) {
                    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
                        std::find(compared.begin(), compared.end(), tab.compare->path) == compared.end()) {
                      compared.push_back(tab.compare->path);
                    }
                  }
                }
                for (const std::filesystem::path& path : compared) {
                  RefreshOpenCompareTabsForPath(path);
                }
              },
          .reload_clean_open_buffers = [this]() { ReloadCleanOpenBuffersFromDisk(); },
          .save_config_state = [this]() { MakePersistenceCoordinator().SaveConfigState(); },
          .save_session_state = [this]() { MakePersistenceCoordinator().SaveSessionState(); },
          .save_workspace_session =
              [this]() { MakePersistenceCoordinator().SaveWorkspaceSession(); },
          .shutdown_plugin_host = [this]() { plugin_runtime_.ShutdownHost(); },
          .reset_project_catalog_to_welcome_state =
              [this]() { ResetProjectCatalogToWelcomeState(); },
          .ensure_active_project_visible = [this]() { tab_strip_chrome_.EnsureActiveProjectVisible(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
      });
  return *glue_->project_catalog_service;
}

bool WorkspaceShell::HasActiveProjectCatalogEntry() const {
  return context_.HasActiveProjectCatalogEntry();
}

WorkspaceShell::ProjectWorkspaceState* WorkspaceShell::ProjectCatalogEntry(std::size_t index) {
  return context_.ProjectCatalogEntry(index);
}

const std::filesystem::path& WorkspaceShell::ProjectCatalogRoot(std::size_t index) const {
  return context_.ProjectCatalogRoot(index);
}

void WorkspaceShell::ResetProjectCatalogToWelcomeState() {
  context_.project_catalog.active_index = 0;
  context_.project_catalog.tab_scroll_index = 0;
  ResetProjectScopedState(true);
  (void)StartFileIndexWatcherForCurrentProject();
  ReloadPluginsForCurrentProject();
  RequestPreparedFrame();
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
    if (PublishProjectRootPresence(notification_service_, context_.current_project_state.root, SDL_GetTicks())) {
      RequestFullRedraw();
    }
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
  if (ProjectWorkspaceState* state = context_.ProjectStateForRoot(closed_root); state != nullptr) {
    MakeTerminalPanelService().ReleaseHostTerminals(*state);
  }
  MakeProjectCatalogService().Close(index);
  if (glue_->remote_host_service != nullptr) {
    glue_->remote_host_service->ProjectClosed(closed_root);
  }
  NoteLayoutInputsChanged();
}

bool WorkspaceShell::ProjectTabStripVisible() const {
  if (context_.project_catalog.entries.size() > 1) {
    return true;
  }
  const std::uint64_t settings_revision = settings_store_.Revision();
  if (settings_revision != project_tabs_hide_when_single_revision_) {
    project_tabs_hide_when_single_revision_ = settings_revision;
    project_tabs_hide_when_single_ =
        SettingFlagEnabled(GetSettingValue("chrome.project_tabs.hide_when_single"),
                           true);
  }
  return !project_tabs_hide_when_single_;
}

}
