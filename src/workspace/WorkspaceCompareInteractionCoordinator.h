#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

class CompareInteractionCoordinator {
 public:
  struct Operations {
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> show_compare_picker_overlay;
    std::function<void()> reset_overlay_scroll;
    std::function<void()> request_overlay_redraw;
    std::function<void()> reveal_compare_picker_selection;
    std::function<void(const project::GitCommitEntry&)> open_comparison;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void()> reveal_active_compare_selection;
    std::function<void(std::size_t, std::size_t)> request_compare_row_range_redraw;
    std::function<void(CompareTabState&)> sync_compare_viewport_scroll;
    std::function<void(CompareTabState&, int)> scroll_compare_rows;
    std::function<void(CompareTabState&, int)> scroll_compare_columns;
    std::function<void()> request_editor_surface_redraw;
    std::function<void()> reveal_active_merge_selection;
    std::function<void(std::size_t)> request_merge_conflict_redraw;
    std::function<void(MergeTabState&, int)> scroll_merge_columns;
    std::function<void(MergeTabState&, const std::vector<std::string>&)>
        update_merge_max_visual_columns;
    std::function<void(std::size_t)> request_merge_result_line_to_bottom_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
  };

  CompareInteractionCoordinator(ProjectWorkspaceState& state, Operations operations);

  void OpenPicker();
  bool OpenPickerForPath(const std::filesystem::path& path,
                         std::string_view commit_spec = {});
  void RefreshPicker();
  void MovePickerSelection(int delta);
  void OpenSelectedCommit();
  void OpenWorkingFileFromCompare();
  void OpenMergeResultFile();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  void MoveMergeSelection(int delta);
  void ScrollMergeColumns(int delta);
 void ApplyMergeChoice(compare::MergeChoice choice);

 private:
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
