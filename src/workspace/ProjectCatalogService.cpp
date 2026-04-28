#include "workspace/ProjectCatalogService.h"

#include <utility>

namespace microide::workspace {

ProjectCatalogService::ProjectCatalogService(ProjectCatalogCoordinator coordinator)
    : coordinator_(std::move(coordinator)) {}

bool ProjectCatalogService::Open(const std::filesystem::path& normalized_root,
                                 bool restore_persistence,
                                 bool log_feedback) {
  return coordinator_.Open(normalized_root, restore_persistence, log_feedback);
}

bool ProjectCatalogService::Switch(std::size_t index, bool activate_restored_tab) {
  return coordinator_.Switch(index, activate_restored_tab);
}

void ProjectCatalogService::Close(std::size_t index, bool activate_restored_tab) {
  coordinator_.Close(index, activate_restored_tab);
}

bool ProjectCatalogService::RestoreAfterRemoval(std::size_t preferred_index,
                                                bool activate_restored_tab) {
  return coordinator_.RestoreAfterRemoval(preferred_index, activate_restored_tab);
}

void ProjectCatalogService::PersistActiveEntry() {
  coordinator_.PersistActiveEntry();
}

void ProjectCatalogService::PersistInactiveEntriesForShutdown() {
  coordinator_.PersistInactiveEntriesForShutdown();
}

}  // namespace microide::workspace
