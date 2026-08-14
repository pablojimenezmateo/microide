#include "workspace/services/EditorTabService.h"

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

bool EditorTabService::SaveGroupTab(std::size_t group_index, std::size_t index) {
  return coordinator_.SaveGroupTab(group_index, index);
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

bool EditorTabService::HasDirtyTabForProject(std::size_t project_index) const {
  return coordinator_.HasDirtyTabForProject(project_index);
}

std::vector<GroupTabRef> EditorTabService::DirtyGroupTabs() const {
  return coordinator_.DirtyGroupTabs();
}

std::vector<GroupTabRef> EditorTabService::DirtyGroupTabsForProject(std::size_t project_index) const {
  return coordinator_.DirtyGroupTabsForProject(project_index);
}

bool EditorTabService::ActiveTabIsEditor() const {
  return coordinator_.ActiveTabIsEditor();
}

TabEntry::EditorTabState* EditorTabService::ActiveEditorTab() {
  return coordinator_.ActiveEditorTab();
}

const TabEntry::EditorTabState* EditorTabService::ActiveEditorTab() const {
  return coordinator_.ActiveEditorTab();
}

editor::TextViewport* EditorTabService::ActiveEditorViewport() {
  return coordinator_.ActiveEditorViewport();
}

const editor::TextViewport* EditorTabService::ActiveEditorViewport() const {
  return coordinator_.ActiveEditorViewport();
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

void EditorTabService::ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path) {
  coordinator_.ReloadEditorTabsForPathFromDisk(path);
}

bool EditorTabService::OverwriteEditorTabsForPath(const std::filesystem::path& path) {
  return coordinator_.OverwriteEditorTabsForPath(path);
}

bool EditorTabService::DiskSignatureMatchesOpenView(const std::filesystem::path& path,
                                                    const util::FileSignature& signature) const {
  return coordinator_.DiskSignatureMatchesOpenView(path, signature);
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

void EditorTabService::CloseGroupTab(std::size_t group_index, std::size_t index) {
  coordinator_.CloseGroupTab(group_index, index);
}

bool EditorTabService::SplitEditorGroup(EditorSplitOrientation orientation) {
  return coordinator_.SplitEditorGroup(orientation);
}

bool EditorTabService::FocusOtherGroup() {
  return coordinator_.FocusOtherGroup();
}

bool EditorTabService::CloseEditorGroup() {
  return coordinator_.CloseEditorGroup();
}

std::size_t EditorTabService::EditorGroupCount() const {
  return coordinator_.EditorGroupCount();
}

bool EditorTabService::MoveActiveTo(std::size_t index) {
  return coordinator_.MoveActiveTo(index);
}

bool EditorTabService::MoveTabToGroup(std::size_t from_group,
                                      std::size_t from_index,
                                      std::size_t to_group,
                                      std::size_t to_slot) {
  return coordinator_.MoveTabToGroup(from_group, from_index, to_group, to_slot);
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
