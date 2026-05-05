#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <chrono>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/ProjectCatalogService.h"

namespace microide::workspace {

ProjectCatalogService WorkspaceShell::MakeProjectCatalogService() {
  return ProjectCatalogService(
      context_,
      ProjectCatalogService::Operations{
          .initialize_current_project =
              [this](const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback,
                     bool activate_restored_tab) {
                return InitializeCurrentProject(project_root,
                                                restore_persistence,
                                                log_feedback,
                                                activate_restored_tab);
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
          .set_project_file_monitor_root =
              [this](const std::filesystem::path& root) {
                project_file_monitor_.SetDeferredArming(true);
                project_file_monitor_.SetProjectRoot(root);
                project_file_monitor_.SetDeferredArming(false);
                project_file_monitor_.SetPollInterval(std::chrono::milliseconds(2000));
              },
          .apply_colorscheme =
              [this]() {
                MakePersistenceCoordinator().ApplyColorscheme(
                    context_.current_project_state.active_colorscheme_name, false, false);
              },
          .apply_editor_preferences_to_all_tabs =
              [this]() { ApplyEditorPreferencesToAllTabs(); },
          .apply_welcome_editor_preferences_if_placeholder =
              [this]() {
                if (context_.current_project_state.welcome_surface.viewport.is_placeholder()) {
                  ApplyEditorPreferences(context_.current_project_state.welcome_surface.viewport);
                }
              },
          .ensure_terminal_tab_open =
              [this]() {
                if (!context_.current_project_state.root.empty() &&
                    context_.current_project_state.terminal_tabs.empty()) {
                  OpenTerminal({}, true, false);
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
          .ensure_active_project_visible = [this]() { EnsureActiveProjectVisible(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
      });
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

std::filesystem::path WorkspaceShell::ProjectCatalogRoot(std::size_t index) const {
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
                                    bool log_feedback) {
  const std::filesystem::path normalized_root = ResolveProjectRootInput(project_root);
  if (normalized_root.empty()) {
    return false;
  }

  if (!context_.current_project_state.root.empty() && normalized_root == context_.current_project_state.root) {
    EnsureActiveProjectVisible();
    return true;
  }

  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (ProjectCatalogRoot(i) == normalized_root) {
      const bool switched = SwitchProject(i, log_feedback);
      if (switched) {
        MarkLayoutDirty();
      }
      return switched;
    }
  }

  const bool opened =
      MakeProjectCatalogService().Open(normalized_root, restore_persistence, log_feedback);
  if (opened) {
    MarkLayoutDirty();
  }
  return opened;
}

bool WorkspaceShell::SwitchProject(std::size_t index, bool log_feedback) {
  (void) log_feedback;
  if (index >= context_.project_catalog.entries.size()) {
    return false;
  }
  MakeMenuCoordinator().CloseTreeContextMenu();
  if (HasActiveProjectCatalogEntry() && index == context_.project_catalog.active_index) {
    EnsureActiveProjectVisible();
    return true;
  }
  const bool switched = MakeProjectCatalogService().Switch(index);
  if (switched) {
    MarkLayoutDirty();
  }
  return switched;
}

bool WorkspaceShell::MoveActiveProjectTo(std::size_t index) {
  if (context_.project_catalog.active_index >= context_.project_catalog.entries.size() || index >= context_.project_catalog.entries.size()) {
    return false;
  }
  if (context_.project_catalog.active_index == index) {
    return true;
  }

  std::unique_ptr<ProjectWorkspaceState> moved_project =
      std::move(context_.project_catalog.entries[context_.project_catalog.active_index]);
  context_.project_catalog.entries.erase(context_.project_catalog.entries.begin() + static_cast<std::ptrdiff_t>(context_.project_catalog.active_index));
  context_.project_catalog.entries.insert(context_.project_catalog.entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved_project));
  context_.project_catalog.active_index = index;
  EnsureActiveProjectVisible();
  return true;
}

void WorkspaceShell::RequestCloseProject(std::size_t index) {
  if (index >= context_.project_catalog.entries.size()) {
    return;
  }
  if (!DirtyEditorTabIndicesForProject(index).empty()) {
    ShowDirtyPromptForProject(index);
    return;
  }
  CloseProject(index);
}

void WorkspaceShell::CloseProject(std::size_t index) {
  MakeProjectCatalogService().Close(index);
}

}  // namespace microide::workspace
