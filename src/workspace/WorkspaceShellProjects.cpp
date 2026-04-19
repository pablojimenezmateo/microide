#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HasActiveProjectCatalogEntry() const {
  return !project_root_.empty() && project_catalog_.active_index < project_catalog_.entries.size();
}

WorkspaceShell::ProjectWorkspaceState* WorkspaceShell::ProjectCatalogEntry(std::size_t index) {
  return index < project_catalog_.entries.size() ? project_catalog_.entries[index].get() : nullptr;
}

const WorkspaceShell::ProjectWorkspaceState* WorkspaceShell::ProjectCatalogEntry(
    std::size_t index) const {
  return index < project_catalog_.entries.size() ? project_catalog_.entries[index].get() : nullptr;
}

std::filesystem::path WorkspaceShell::ProjectCatalogRoot(std::size_t index) const {
  if (index >= project_catalog_.entries.size()) {
    return {};
  }
  if (!project_root_.empty() && index == project_catalog_.active_index) {
    return project_root_;
  }
  const auto* state = ProjectCatalogEntry(index);
  return state != nullptr ? state->root : std::filesystem::path{};
}

void WorkspaceShell::ResetProjectCatalogToWelcomeState() {
  project_catalog_.active_index = 0;
  project_catalog_.tab_scroll_index = 0;
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

  if (!project_root_.empty() && normalized_root == project_root_) {
    EnsureActiveProjectVisible();
    return true;
  }

  for (std::size_t i = 0; i < project_catalog_.entries.size(); ++i) {
    if (ProjectCatalogRoot(i) == normalized_root) {
      return SwitchProject(i, log_feedback);
    }
  }

  return ProjectCatalogCoordinator(*this).Open(normalized_root, restore_persistence, log_feedback);
}

bool WorkspaceShell::SwitchProject(std::size_t index, bool log_feedback) {
  (void) log_feedback;
  if (index >= project_catalog_.entries.size()) {
    return false;
  }
  MenuCoordinator(*this).CloseTreeContextMenu();
  if (HasActiveProjectCatalogEntry() && index == project_catalog_.active_index) {
    EnsureActiveProjectVisible();
    return true;
  }

  return ProjectCatalogCoordinator(*this).Switch(index);
}

bool WorkspaceShell::MoveActiveProjectTo(std::size_t index) {
  if (project_catalog_.active_index >= project_catalog_.entries.size() || index >= project_catalog_.entries.size()) {
    return false;
  }
  if (project_catalog_.active_index == index) {
    return true;
  }

  std::unique_ptr<ProjectWorkspaceState> moved_project =
      std::move(project_catalog_.entries[project_catalog_.active_index]);
  project_catalog_.entries.erase(project_catalog_.entries.begin() + static_cast<std::ptrdiff_t>(project_catalog_.active_index));
  project_catalog_.entries.insert(project_catalog_.entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved_project));
  project_catalog_.active_index = index;
  EnsureActiveProjectVisible();
  return true;
}

void WorkspaceShell::RequestCloseProject(std::size_t index) {
  if (index >= project_catalog_.entries.size()) {
    return;
  }
  if (!DirtyEditorTabIndicesForProject(index).empty()) {
    ShowDirtyPromptForProject(index);
    return;
  }
  CloseProject(index);
}

void WorkspaceShell::CloseProject(std::size_t index) {
  ProjectCatalogCoordinator(*this).Close(index);
}

}  // namespace microide::workspace
