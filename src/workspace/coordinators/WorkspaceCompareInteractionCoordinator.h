#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class CompareInteractionCoordinator {
 public:
  struct Operations {
    std::function<void()> show_compare_picker_overlay;
    std::function<void()> reset_overlay_scroll;
    std::function<void()> reveal_compare_picker_selection;
    std::function<void(const project::GitCommitEntry&)> open_comparison;
    std::function<void(const std::string& ref, const std::string& label)> set_outgoing_base_ref;
    std::function<void(const std::string& branch)> switch_to_branch;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void()> reveal_active_compare_selection;
    std::function<void()> request_frame;
    std::function<void(CompareTabState&)> sync_compare_viewport_scroll;
    std::function<void(CompareTabState&, int)> scroll_compare_rows;
    std::function<void(CompareTabState&, int)> scroll_compare_columns;
    std::function<void()> reveal_active_merge_selection;
    std::function<void(MergeTabState&, const std::vector<std::string>&)>
        update_merge_max_visual_columns;
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&)>
        open_working_tree_comparison;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&,
                       const std::string&, const std::string&,
                       const std::vector<std::filesystem::path>*)>
        open_branch_head_comparison;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<bool()> save_active_merge_tab;
    std::function<bool(const std::filesystem::path&)> stage_merge_result_path;
    std::function<void()> refresh_git_sidebar;
    std::function<void(const std::filesystem::path&)> request_compare_file_history;
    std::function<void(bool include_commits)> request_ref_list;
  };

  CompareInteractionCoordinator(ProjectWorkspaceState& state, Operations operations);

  bool OpenPickerForPath(const std::filesystem::path& path,
                         std::string_view commit_spec = {});
  void OpenOutgoingBasePicker();
  void OpenBranchSwitchPicker();
  void ApplyFileHistoryResult(const project::GitFileHistoryResult& history);
  void ApplyRefsResult(const std::vector<project::GitBranchReference>& branches,
                       const std::vector<project::GitCommitEntry>& commits);
  void RefreshPicker();
  void MovePickerSelection(int delta);
  void OpenSelectedCommit();
  void OpenWorkingFileFromCompare();
  void OpenMergeResultFile();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void JumpCompareReviewFile(int delta);
  void CopyComparePath();
  void CopyCompareHunkPatch();
  void CopyCompareFilePatch();
  void ToggleCompareIgnoreWhitespace();
  void ToggleCompareShowWhitespace();
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  void MoveMergeSelection(int delta);
 void ApplyMergeChoice(compare::MergeChoice choice);
  void ResetMergeHunk();
  void JumpNextUnresolvedMergeConflict();
  void ToggleMergeBasePane();
  void CopyMergeSideSnippet(bool incoming);
  void MarkMergeResolved();

 private:
  ProjectWorkspaceState& state_;
  Operations operations_;
};

}
