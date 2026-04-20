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
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<TabEntry::EditorTabState(const editor::TextViewport&)> make_editor_tab_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState::EditorViewState&)>
        editor_view_path;
    std::function<void(TabEntry::EditorTabState&)> normalize_editor_split_tree;
    std::function<void()> sync_active_editor_tab_metadata;
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
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  bool OpenUntitled();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool MoveActiveTo(std::size_t index);
  std::optional<std::size_t> FindIndexBySpecifier(std::string_view specifier,
                                                  std::string* error_message) const;
  bool ReopenActive();

  static bool TabStateIsDirty(const TabEntry& tab);

 private:
  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
