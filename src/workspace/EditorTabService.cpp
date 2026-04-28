#include "workspace/EditorTabService.h"

#include <utility>

namespace microide::workspace {

EditorTabService::EditorTabService(TabCoordinator coordinator)
    : coordinator_(std::move(coordinator)) {}

std::string EditorTabService::ActiveTitle() const {
  return coordinator_.ActiveTitle();
}

bool EditorTabService::Save(std::size_t index) {
  return coordinator_.Save(index);
}

bool EditorTabService::IsDirty(std::size_t index) const {
  return coordinator_.IsDirty(index);
}

std::vector<std::size_t> EditorTabService::DirtyIndices() const {
  return coordinator_.DirtyIndices();
}

std::vector<std::size_t> EditorTabService::DirtyIndicesForProject(std::size_t project_index) const {
  return coordinator_.DirtyIndicesForProject(project_index);
}

void EditorTabService::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  coordinator_.ReloadCleanEditorTabsForPath(path);
}

bool EditorTabService::OpenUntitled() {
  return coordinator_.OpenUntitled();
}

bool EditorTabService::OpenFileInNewTab(const std::filesystem::path& path) {
  return coordinator_.OpenFileInNewTab(path);
}

bool EditorTabService::MoveActiveTo(std::size_t index) {
  return coordinator_.MoveActiveTo(index);
}

std::optional<std::size_t> EditorTabService::FindIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return coordinator_.FindIndexBySpecifier(specifier, error_message);
}

bool EditorTabService::ReopenActive() {
  return coordinator_.ReopenActive();
}

}  // namespace microide::workspace
