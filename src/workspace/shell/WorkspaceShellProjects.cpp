#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <chrono>

#include "workspace/SettingFlags.h"
#include "workspace/TabReorder.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/ProjectCatalogService.h"

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
                  OpenDefaultTerminalForProjectInit();
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
                                    bool log_feedback) {
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

  const bool opened =
      MakeProjectCatalogService().Open(normalized_root, restore_persistence, log_feedback);
  if (opened) {
    recents_service_.RecordProjectOpen(normalized_root);
    NoteLayoutInputsChanged();
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
  MakeProjectCatalogService().Close(index);
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
