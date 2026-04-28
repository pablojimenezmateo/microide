#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/ProjectCatalogService.h"

namespace microide::workspace {

ProjectCatalogCoordinator WorkspaceShell::MakeProjectCatalogCoordinator() {
  return ProjectCatalogCoordinator(
      context_,
      ProjectCatalogCoordinator::Operations{
          .initialize_current_project =
              [this](const std::filesystem::path& project_root,
                     bool restore_persistence,
                     bool log_feedback) {
                return InitializeCurrentProject(project_root, restore_persistence, log_feedback);
              },
          .activate_project_state =
              [this](ProjectWorkspaceState& state, bool activate_restored_tab) {
                return ActivateProjectState(state, activate_restored_tab);
              },
          .store_current_project_state =
              [this](ProjectWorkspaceState& state) { StoreCurrentProjectState(state); },
          .load_project_state = [this](ProjectWorkspaceState& state) { LoadProjectState(state); },
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

ProjectCatalogService WorkspaceShell::MakeProjectCatalogService() {
  return ProjectCatalogService(MakeProjectCatalogCoordinator());
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
      return SwitchProject(i, log_feedback);
    }
  }

  return MakeProjectCatalogService().Open(normalized_root, restore_persistence, log_feedback);
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

  return MakeProjectCatalogService().Switch(index);
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
