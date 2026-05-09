#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

class TabCoordinator {
 public:
  struct Operations {
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_save;
    std::function<void(const std::filesystem::path&)> notify_plugin_buffer_open;
    std::function<void(const std::filesystem::path&)> notify_lsp_buffer_close;
    std::function<std::size_t(const std::filesystem::path&)> count_open_buffer_views;
    std::function<bool(const std::filesystem::path&, editor::TextViewport&, std::string*)>
        prepare_editor_view_for_save;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(editor::TextViewport&)> apply_detected_indent_on_open;
    std::function<TabEntry::EditorTabState(const editor::TextViewport&)> make_editor_tab_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState::EditorViewState&)>
        editor_view_path;
    std::function<editor::TextViewport*(TabEntry::EditorTabState&, std::size_t)> find_editor_view;
    std::function<void(TabEntry::EditorTabState&)> normalize_editor_split_tree;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> reveal_active_compare_selection;
    std::function<void()> reveal_active_merge_selection;
    std::function<void()> ensure_active_tab_visible;
    std::function<void()> reset_caret_blink;
    std::function<void(bool)> request_active_tab_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void()> request_editor_surface_redraw;
    std::function<void(std::size_t)> activate_tab;
  };

  TabCoordinator(ProjectCatalogState& project_catalog,
                 ProjectWorkspaceState& current_project_state,
                 Operations operations);

  std::string ActiveTitle() const;
  bool Save(std::size_t index);
  bool IsDirty(std::size_t index) const;
  std::vector<std::size_t> DirtyIndices() const;
  std::vector<std::size_t> DirtyIndicesForProject(std::size_t project_index) const;
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  void Activate(std::size_t index);
  void SyncActiveEditorTab();
  bool ActivateCurrentTabAfterStateLoad();
  void SyncActiveEditorTabMetadata();
  void SetActiveEditorSplit(std::size_t leaf_id);
  bool ActivateOrderedEditorSplit(std::size_t order_index);
  bool SplitActiveEditor(EditorSplitOrientation orientation);
  bool UnsplitActiveEditor();
  bool CycleEditorSplit(int delta);
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  bool OpenUntitled();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                   std::string_view content,
                                   std::string_view title);
  void ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                 std::string_view content);
  void Close(std::size_t index);
  bool MoveActiveTo(std::size_t index);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();

  static bool TabStateIsDirty(const TabEntry& tab);

 private:
  struct EditorSplitSlot {
    TabEntry::EditorTabState::EditorSplitNode* parent = nullptr;
    std::size_t index = 0;
    std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode>* slot = nullptr;
  };

  static std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode> MakeEditorLeafNode(
      std::size_t leaf_id,
      float size_fraction = 1.0f);
  EditorSplitSlot FindEditorLeafSlot(TabEntry::EditorTabState& editor_tab,
                                     std::size_t leaf_id);
  bool RestoreEditorView(TabEntry::EditorTabState::EditorViewState& view);
  TabEntry::EditorTabState::EditorViewState* FindEditorViewState(
      TabEntry::EditorTabState& editor_tab,
      std::size_t leaf_id);
  const TabEntry::EditorTabState::EditorViewState* FindEditorViewState(
      const TabEntry::EditorTabState& editor_tab,
      std::size_t leaf_id) const;
  void CollectEditorLeafOrder(const TabEntry::EditorTabState::EditorSplitNode* node,
                              std::vector<std::size_t>& order) const;
  std::vector<std::size_t> EditorLeafOrder(const TabEntry::EditorTabState& editor_tab) const;
  bool EnsureEditorTabLoaded(TabEntry& tab);

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
