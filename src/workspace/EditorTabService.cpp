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

void EditorTabService::Activate(std::size_t index) {
  coordinator_.Activate(index);
}

void EditorTabService::SyncActiveEditorTab() {
  coordinator_.SyncActiveEditorTab();
}

bool EditorTabService::ActivateCurrentTabAfterStateLoad() {
  return coordinator_.ActivateCurrentTabAfterStateLoad();
}

void EditorTabService::SyncActiveEditorTabMetadata() {
  coordinator_.SyncActiveEditorTabMetadata();
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

bool EditorTabService::OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                                   std::string_view content,
                                                   std::string_view title) {
  return coordinator_.OpenVirtualDocumentInNewTab(virtual_path, content, title);
}

void EditorTabService::ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                                 std::string_view content) {
  coordinator_.ReloadVirtualDocumentTabs(virtual_path, content);
}

void EditorTabService::Close(std::size_t index) {
  coordinator_.Close(index);
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
