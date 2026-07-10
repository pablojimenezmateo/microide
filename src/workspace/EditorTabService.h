#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceTabCoordinator.h"

namespace microide::workspace {

class EditorTabService {
 public:
  using DeferredTabHandle = TabEntry::DeferredTabHandle;
  explicit EditorTabService(TabCoordinator coordinator);

  std::string ActiveTitle() const;
  bool Save(std::size_t index);
  bool SaveGroupTab(std::size_t group_index, std::size_t index);
  bool IsDirty(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  std::vector<GroupTabRef> DirtyGroupTabs() const;
  std::vector<GroupTabRef> DirtyGroupTabsForProject(std::size_t project_index) const;
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  void Activate(std::size_t index);
  void SyncActiveEditorTab();
  bool ActivateCurrentTabAfterStateLoad();
  void SyncActiveEditorTabMetadata();
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  void ReloadEditorTabsForPathFromDisk(const std::filesystem::path& path);
  bool OverwriteEditorTabsForPath(const std::filesystem::path& path);
  bool DiskSignatureMatchesOpenView(const std::filesystem::path& path,
                                    const util::FileSignature& signature) const;
  bool OpenUntitled();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                   std::string_view content,
                                   std::string_view title);
  void ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                 std::string_view content);
  void Close(std::size_t index);
  void CloseGroupTab(std::size_t group_index, std::size_t index);
  bool SplitEditorGroup(EditorSplitOrientation orientation);
  bool FocusOtherGroup();
  bool CloseEditorGroup();
  std::size_t EditorGroupCount() const;
  bool MoveActiveTo(std::size_t index);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();

 private:
  TabCoordinator coordinator_;
};

}  // namespace microide::workspace
