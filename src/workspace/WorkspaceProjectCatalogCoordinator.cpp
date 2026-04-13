#include "workspace/WorkspaceProjectCatalogCoordinator.h"

#include <filesystem>
#include <memory>

namespace microide::workspace {

WorkspaceShell::ProjectCatalogCoordinator::ProjectCatalogCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::ProjectCatalogCoordinator::Open(const std::filesystem::path& normalized_root,
                                                     bool restore_persistence,
                                                     bool log_feedback) {
  const ActivationCheckpoint checkpoint = CaptureActivationCheckpoint();

  auto project_state = std::make_unique<ProjectWorkspaceState>();
  project_state->root = normalized_root;
  project_state->initialized = true;
  shell_.project_catalog_.entries.push_back(std::move(project_state));
  shell_.project_catalog_.active_index = shell_.project_catalog_.entries.size() - 1;

  if (!shell_.InitializeCurrentProject(normalized_root, restore_persistence, log_feedback)) {
    shell_.project_catalog_.entries.pop_back();
    RestoreActivationCheckpoint(checkpoint);
    return false;
  }

  FinalizeMutation();
  return true;
}

bool WorkspaceShell::ProjectCatalogCoordinator::Switch(std::size_t index,
                                                       bool activate_restored_tab) {
  const ActivationCheckpoint checkpoint = CaptureActivationCheckpoint();
  if (!Activate(index, activate_restored_tab)) {
    RestoreActivationCheckpoint(checkpoint);
    return false;
  }

  FinalizeMutation();
  return true;
}

void WorkspaceShell::ProjectCatalogCoordinator::Close(std::size_t index,
                                                      bool activate_restored_tab) {
  if (index >= shell_.project_catalog_.entries.size()) {
    return;
  }

  const bool closing_active =
      shell_.HasActiveProjectCatalogEntry() && index == shell_.project_catalog_.active_index;
  if (closing_active) {
    PersistActiveEntry();
  }

  shell_.project_catalog_.entries.erase(
      shell_.project_catalog_.entries.begin() + static_cast<std::ptrdiff_t>(index));
  if (shell_.project_catalog_.entries.empty()) {
    shell_.ResetProjectCatalogToWelcomeState();
    shell_.SaveWorkspaceSession();
    return;
  }

  if (closing_active) {
    if (!RestoreAfterRemoval(index, activate_restored_tab)) {
      shell_.SaveWorkspaceSession();
      return;
    }
  } else if (shell_.project_catalog_.active_index > index) {
    --shell_.project_catalog_.active_index;
  }

  FinalizeMutation();
}

bool WorkspaceShell::ProjectCatalogCoordinator::RestoreAfterRemoval(std::size_t preferred_index,
                                                                    bool activate_restored_tab) {
  while (!shell_.project_catalog_.entries.empty()) {
    const std::size_t index =
        std::min(preferred_index, shell_.project_catalog_.entries.size() - 1);
    if (Activate(index, activate_restored_tab)) {
      return true;
    }
    shell_.project_catalog_.entries.erase(
        shell_.project_catalog_.entries.begin() + static_cast<std::ptrdiff_t>(index));
    preferred_index = index;
  }

  shell_.ResetProjectCatalogToWelcomeState();
  return false;
}

void WorkspaceShell::ProjectCatalogCoordinator::PersistActiveEntry() {
  if (!shell_.HasActiveProjectCatalogEntry()) {
    return;
  }
  shell_.SaveConfigState();
  shell_.SaveSessionState();
  shell_.StoreCurrentProjectState(*shell_.project_catalog_.entries[shell_.project_catalog_.active_index]);
}

void WorkspaceShell::ProjectCatalogCoordinator::PersistInactiveEntriesForShutdown() {
  for (std::size_t i = 0; i < shell_.project_catalog_.entries.size(); ++i) {
    auto* entry = shell_.ProjectCatalogEntry(i);
    if (entry == nullptr || !entry->initialized ||
        (shell_.HasActiveProjectCatalogEntry() && i == shell_.project_catalog_.active_index)) {
      continue;
    }
    shell_.LoadProjectState(*entry);
    shell_.SaveConfigState();
    shell_.SaveSessionState();
    shell_.StoreCurrentProjectState(*entry);
  }
}

bool WorkspaceShell::ProjectCatalogCoordinator::Activate(std::size_t index,
                                                         bool activate_restored_tab) {
  auto* entry = shell_.ProjectCatalogEntry(index);
  if (entry == nullptr) {
    return false;
  }
  shell_.project_catalog_.active_index = index;
  return shell_.ActivateProjectState(*entry, activate_restored_tab);
}

WorkspaceShell::ProjectCatalogCoordinator::ActivationCheckpoint
WorkspaceShell::ProjectCatalogCoordinator::CaptureActivationCheckpoint() {
  ActivationCheckpoint checkpoint = {
      .had_active_project = shell_.HasActiveProjectCatalogEntry(),
      .previous_active_index = shell_.project_catalog_.active_index,
  };
  if (checkpoint.had_active_project) {
    PersistActiveEntry();
  }
  return checkpoint;
}

void WorkspaceShell::ProjectCatalogCoordinator::RestoreActivationCheckpoint(
    const ActivationCheckpoint& checkpoint) {
  if (checkpoint.had_active_project &&
      checkpoint.previous_active_index < shell_.project_catalog_.entries.size() &&
      Activate(checkpoint.previous_active_index, true)) {
    return;
  }
  shell_.ResetProjectCatalogToWelcomeState();
}

void WorkspaceShell::ProjectCatalogCoordinator::FinalizeMutation() {
  shell_.EnsureActiveProjectVisible();
  shell_.SaveWorkspaceSession();
}

}  // namespace microide::workspace
