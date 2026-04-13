#pragma once

#include <filesystem>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::ProjectCatalogCoordinator {
 public:
  explicit ProjectCatalogCoordinator(WorkspaceShell& shell);

  bool Open(const std::filesystem::path& normalized_root,
            bool restore_persistence,
            bool log_feedback);
  bool Switch(std::size_t index, bool activate_restored_tab = true);
  void Close(std::size_t index, bool activate_restored_tab = true);
  bool RestoreAfterRemoval(std::size_t preferred_index, bool activate_restored_tab = true);
  void PersistActiveEntry();
  void PersistInactiveEntriesForShutdown();

 private:
  struct ActivationCheckpoint {
    bool had_active_project = false;
    std::size_t previous_active_index = 0;
  };

  bool Activate(std::size_t index, bool activate_restored_tab);
  ActivationCheckpoint CaptureActivationCheckpoint();
  void RestoreActivationCheckpoint(const ActivationCheckpoint& checkpoint);
  void FinalizeMutation();

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
