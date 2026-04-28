#pragma once

#include <cstddef>
#include <filesystem>

#include "workspace/WorkspaceProjectCatalogCoordinator.h"

namespace microide::workspace {

class ProjectCatalogService {
 public:
  explicit ProjectCatalogService(ProjectCatalogCoordinator coordinator);

  bool Open(const std::filesystem::path& normalized_root,
            bool restore_persistence,
            bool log_feedback);
  bool Switch(std::size_t index, bool activate_restored_tab = true);
  void Close(std::size_t index, bool activate_restored_tab = true);
  bool RestoreAfterRemoval(std::size_t preferred_index, bool activate_restored_tab = true);
  void PersistActiveEntry();
  void PersistInactiveEntriesForShutdown();

 private:
  ProjectCatalogCoordinator coordinator_;
};

}  // namespace microide::workspace
