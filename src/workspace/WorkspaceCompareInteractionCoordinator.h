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
    // Sets the sidebar outgoing-comparison base to the picked ref (branch name or
    // commit hash) with a user-facing label.
    std::function<void(const std::string& ref, const std::string& label)> set_outgoing_base_ref;
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
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&)>
        open_working_tree_comparison;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&,
                       const std::string&, const std::string&)>
        open_branch_head_comparison;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void()> stage_compare_hunk;
    std::function<void()> stage_compare_selected_lines;
    std::function<void()> unstage_compare_hunk;
    std::function<void()> unstage_compare_selected_lines;
    std::function<void()> open_discard_compare_hunk_prompt;
    std::function<void()> open_discard_compare_selected_lines_prompt;
    std::function<bool()> save_active_merge_tab;
    std::function<bool(const std::filesystem::path&)> stage_merge_result_path;
    std::function<void()> refresh_git_sidebar;
    // Dispatch the (blocking) git file-history / branch+recent-commit queries on
    // the shell's background executor. The shell marshals the result back to the
    // main thread and repopulates the picker via ApplyFileHistoryResult /
    // ApplyOutgoingBaseResult. Kept as host operations so the stack-temporary
    // coordinator never owns the background job.
    std::function<void(const std::filesystem::path&)> request_compare_file_history;
    std::function<void()> request_outgoing_base_refs;
  };

  CompareInteractionCoordinator(ProjectWorkspaceState& state, Operations operations);

  void OpenPicker();
  bool OpenPickerForPath(const std::filesystem::path& path,
                         std::string_view commit_spec = {});
  void OpenOutgoingBasePicker();
  // Populate the picker from an async git result marshaled back to the main
  // thread. Clears `loading`, rebuilds items, and refreshes matches. The shell's
  // completion handler has already verified the request is still current.
  void ApplyFileHistoryResult(const project::GitFileHistoryResult& history);
  void ApplyOutgoingBaseResult(const std::vector<project::GitBranchReference>& branches,
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
  void StageCompareHunk();
  void StageCompareSelectedLines();
  void UnstageCompareHunk();
  void UnstageCompareSelectedLines();
  void OpenDiscardCompareHunkPrompt();
  void OpenDiscardCompareSelectedLinesPrompt();
  void ToggleCompareIgnoreWhitespace();
  void ToggleCompareShowWhitespace();
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  void MoveMergeSelection(int delta);
  void ScrollMergeColumns(int delta);
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

}  // namespace microide::workspace
