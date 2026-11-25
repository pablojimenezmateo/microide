#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/services/EditorTabService.h"
#include "workspace/services/NotificationService.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class PromptSurfaceService;

class PathMutationCoordinator {
 public:
  struct Operations {
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<void()> clear_editor_blame;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<bool()> discard_all_git_sidebar_entries;
    std::function<bool(std::size_t, const std::filesystem::path&)> discard_git_sidebar_entry;
    std::function<bool()> confirm_discard_patch_preview;
    std::function<void()> cancel_discard_patch_preview;
    std::function<void()> refresh_project_files;
    std::function<void()> request_automatic_git_sidebar_refresh;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<void()> refresh_project_search;
    std::function<void()> refresh_problems_sidebar;
    std::function<void()> queue_editor_hover_refresh;
    std::function<void()> request_editor_surface_redraw;
    std::function<void()> request_window_redraw;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(editor::TextViewport&)> apply_detected_indent_on_open;
    std::function<std::filesystem::path(const TabEntry::EditorTabState&)> editor_view_path;
    std::function<void()> sync_active_editor_tab_metadata;
    std::function<std::optional<TabEntry>(const std::filesystem::path&, const CompareTabState&)>
        build_compare_tab_entry;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&)>
        build_merge_tab_entry;
  };

  PathMutationCoordinator(WorkspaceContext& context,
                          EditorTabService& editor_tabs,
                          PromptSurfaceService& prompt_surfaces,
                          Operations operations);

  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  void ConfirmPromptSurface(DirtyPathResolution resolution);
  std::string PastePath(const std::filesystem::path& source, bool cut, const std::filesystem::path& directory);
  void ResumeDeferredPathMutation(PromptSurfaceState pending);

  void ReconcileAfterExternalRename(const std::filesystem::path& old_path,
                                    const std::filesystem::path& new_path);
  void ReconcileAfterExternalDelete(const std::filesystem::path& path);
  void RefreshViewsAfterExternalMutation(const std::filesystem::path& preferred_tree_path);

 private:
  struct DirtyPathTarget {
    enum class Kind {
      EditorView,
      CompareTab,
      MergeTab,
    };

    Kind kind = Kind::EditorView;
    std::size_t group_index = 0;
    std::size_t tab_index = 0;
  };

  enum class SpecialTabRetargetOutcome {
    Unaffected,
    Retargeted,
    MustClose,
  };

  std::vector<DirtyPathTarget> DirtyPathTargetsForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> DirtyTabIndicesForPath(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedCompareTabIndices(const std::filesystem::path& path) const;
  std::vector<std::size_t> AffectedMergeTabIndices(const std::filesystem::path& path) const;
  static bool CompareTabAffectedByPath(const TabEntry& tab,
                                       const std::filesystem::path& normalized_path);
  static bool MergeTabAffectedByPath(const TabEntry& tab,
                                     const std::filesystem::path& normalized_path);
  SpecialTabRetargetOutcome RetargetSpecialTabForRename(TabEntry& tab,
                                                        const std::filesystem::path& old_path,
                                                        const std::filesystem::path& new_path,
                                                        bool preserve_unsaved_state);
  bool ResolveDirtyTabsForPath(const std::filesystem::path& path,
                               DirtyPromptState::Kind prompt_kind,
                               DirtyPathResolution resolution);
  void RefreshDiagnosticsAfterMutation();
  void RetargetDiagnosticsForRename(const std::filesystem::path& old_path,
                                    const std::filesystem::path& new_path);
  void ClearDiagnosticsForPath(const std::filesystem::path& path);
  void RetargetPluginDecorationsForRename(const std::filesystem::path& old_path,
                                          const std::filesystem::path& new_path);
  void ClearPluginDecorationsForPath(const std::filesystem::path& path);
  void RefreshProjectViewsAfterMutation(const std::filesystem::path& preferred_tree_path);
  void FollowRename(const std::filesystem::path& old_path,
                    const std::filesystem::path& new_path,
                    bool preserve_unsaved_state);
  void RetargetOpenTabsForRename(const std::filesystem::path& old_path,
                                 const std::filesystem::path& new_path,
                                 bool preserve_unsaved_state = true);

  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;
  void FocusSidebarIfShown();

  WorkspaceContext& context_;
  EditorTabService& editor_tabs_;
  PromptSurfaceService& prompt_surfaces_;
  Operations operations_;
};

}
