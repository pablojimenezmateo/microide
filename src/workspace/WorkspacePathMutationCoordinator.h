#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class PathMutationCoordinator {
 public:
  struct Operations {
    std::function<void(bool)> dismiss_prompt_surface;
    std::function<void(bool)> dismiss_dirty_prompt;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<void()> clear_editor_blame;
    std::function<bool()> discard_all_git_sidebar_entries;
    std::function<void()> refresh_project_files;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> refresh_project_search;
    std::function<bool()> active_tab_is_editor;
    std::function<void()> sync_active_editor_tab;
    std::function<bool(std::size_t)> save_tab;
    std::function<void(std::size_t)> close_tab;
    std::function<void()> refresh_problems_sidebar;
    std::function<void()> queue_editor_hover_refresh;
    std::function<void()> request_editor_surface_redraw;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<const editor::TextViewport*(const TabEntry::EditorTabState&, std::size_t)>
        find_editor_view;
    std::function<TabEntry::EditorTabState::EditorViewState*(TabEntry::EditorTabState&, std::size_t)>
        find_editor_view_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState::EditorViewState&)>
        editor_view_path;
    std::function<void(TabEntry::EditorTabState&)> normalize_editor_split_tree;
    std::function<std::vector<std::size_t>(const TabEntry::EditorTabState&)> editor_leaf_order;
    std::function<void()> sync_active_editor_tab_metadata;
    std::function<void()> reset_caret_blink;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<bool(const std::filesystem::path&, std::string*)> has_dirty_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<std::optional<TabEntry>(const std::filesystem::path&, const CompareTabState&)>
        build_compare_tab_entry;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&)>
        build_merge_tab_entry;
  };

  PathMutationCoordinator(WorkspaceContext& context, Operations operations);

  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  void ConfirmPromptSurface(WorkspaceShell::DirtyPathResolution resolution);

 private:
  struct DirtyPathTarget {
    enum class Kind {
      EditorView,
      CompareTab,
      MergeTab,
    };

    Kind kind = Kind::EditorView;
    std::size_t tab_index = 0;
    std::size_t leaf_id = 0;
  };

  std::vector<DirtyPathTarget> DirtyPathTargetsForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> DirtyTabIndicesForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedCompareTabIndices(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedMergeTabIndices(const std::filesystem::path& path) const;
  bool ResolveDirtyTabsForPath(const std::filesystem::path& path,
                               DirtyPromptState::Kind prompt_kind,
                               WorkspaceShell::DirtyPathResolution resolution);
  void RefreshDiagnosticsAfterMutation();
  void RetargetDiagnosticsForRename(const std::filesystem::path& old_path,
                                    const std::filesystem::path& new_path);
  void ClearDiagnosticsForPath(const std::filesystem::path& path);
  void RefreshProjectViewsAfterMutation(const std::filesystem::path& preferred_tree_path);
  void RetargetOpenTabsForRename(const std::filesystem::path& old_path,
                                 const std::filesystem::path& new_path,
                                 bool preserve_unsaved_state = true);

  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  WorkspaceContext& context_;
  Operations operations_;
};

}  // namespace microide::workspace
