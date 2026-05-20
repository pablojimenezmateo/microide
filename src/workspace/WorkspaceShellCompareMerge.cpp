#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/CompareMergeService.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

DiffTabCoordinator WorkspaceShell::MakeDiffTabCoordinator() {
  return DiffTabCoordinator(
      context_.current_project_state,
      DiffTabCoordinator::Operations{
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .notify_plugin_buffer_open =
              [this](const std::filesystem::path& path) { NotifyPluginBufferOpen(path); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
          .dismiss_overlay = [this](bool restore_focus) { DismissOverlay(restore_focus); },
          .request_active_tab_redraw =
              [this](bool include_layout) { RequestActiveTabRedraw(include_layout); },
          .build_compare_tab_entry =
              [this](const std::filesystem::path& path,
                     const project::GitCommitEntry& commit,
                     std::size_t selected_row) {
                return BuildCompareTabEntry(path, commit, selected_row);
              },
          .rebuild_compare_tab_entry =
              [this](const std::filesystem::path& path, const CompareTabState& compare_state) {
                return BuildCompareTabEntry(path, compare_state);
              },
          .build_compare_tab_from_buffers =
              [this](const std::filesystem::path& path,
                     const std::string& left_content,
                     const std::string& right_content,
                     const std::string& left_label,
                     const std::string& right_label,
                     std::size_t selected_row,
                     bool persistable) {
                return BuildCompareTabFromBuffers(path, left_content, right_content, left_label,
                                                  right_label, selected_row, persistable);
              },
          .build_merge_tab_entry =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return BuildMergeTabEntry(base_path, incoming_path, current_path, output_path);
              },
          .build_merge_tab_from_buffers =
              [this](const std::filesystem::path& output_path,
                     const std::string& base_content,
                     const std::string& incoming_content,
                     const std::string& current_content,
                     const std::string& incoming_label,
                     const std::string& result_label,
                     const std::string& current_label,
                     std::size_t selected_hunk,
                     bool persistable) {
                return BuildMergeTabFromBuffers(output_path, base_content, incoming_content,
                                                current_content, incoming_label, result_label,
                                                current_label, selected_hunk, persistable);
              },
      });
}

CompareInteractionCoordinator WorkspaceShell::MakeCompareInteractionCoordinator() {
  return CompareInteractionCoordinator(
      context_.current_project_state,
      CompareInteractionCoordinator::Operations{
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .show_compare_picker_overlay = [this]() { ShowOverlay(OverlayMode::CommitPicker); },
          .reset_overlay_scroll = [this]() { ResetOverlayScroll(); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .reveal_compare_picker_selection =
              [this]() {
                if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
                  RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
                }
              },
          .open_comparison =
              [this](const project::GitCommitEntry& commit) { OpenComparison(commit); },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .request_compare_row_range_redraw =
              [this](std::size_t start, std::size_t end) {
                RequestCompareRowRangeRedraw(start, end);
              },
          .sync_compare_viewport_scroll =
              [this](CompareTabState& compare_tab) { SyncCompareViewportScroll(compare_tab); },
          .scroll_compare_rows =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, compare_tab);
                  compare_tab.scroll_row = std::clamp(scroll_layout.vertical_scroll + delta, 0,
                                                      scroll_layout.max_vertical_scroll);
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .scroll_compare_columns =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, compare_tab);
                  const long long target_scroll =
                      static_cast<long long>(scroll_layout.horizontal_scroll) +
                      static_cast<long long>(delta);
                  compare_tab.horizontal_scroll = static_cast<std::size_t>(std::clamp(
                      target_scroll, 0LL,
                      static_cast<long long>(scroll_layout.max_horizontal_scroll)));
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .request_merge_conflict_redraw =
              [this](std::size_t conflict_index) { RequestMergeConflictRedraw(conflict_index); },
          .scroll_merge_columns =
              [this](MergeTabState& merge_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeMergeSurfaceLayout(layout.editor_surface, merge_tab);
                  const auto scroll_layout =
                      ComputeMergeScrollLayout(layout.editor_surface, surface_layout, merge_tab);
                  const long long target_scroll =
                      static_cast<long long>(scroll_layout.horizontal_scroll) +
                      static_cast<long long>(delta);
                  merge_tab.horizontal_scroll = static_cast<std::size_t>(std::clamp(
                      target_scroll, 0LL,
                      static_cast<long long>(scroll_layout.max_horizontal_scroll)));
                  merge_tab.result_viewport.SetHorizontalScroll(merge_tab.horizontal_scroll);
                  merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
                }
              },
          .update_merge_max_visual_columns =
              [this](MergeTabState& merge_tab, const std::vector<std::string>& replacement_lines) {
                UpdateMergeMaxVisualColumns(merge_tab, replacement_lines);
              },
          .request_merge_result_line_to_bottom_redraw =
              [this](std::size_t start_line) { RequestMergeResultLineToBottomRedraw(start_line); },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before, std::size_t after) {
                RequestActiveEditableBlameNeighborhoodRedraw(before, after);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
      });
}

CompareMergeService WorkspaceShell::MakeCompareMergeService() {
  return CompareMergeService(MakeDiffTabCoordinator(), MakeCompareInteractionCoordinator());
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().FindOpenCompareTabIndex(
      path, left_ref, right_ref);
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().FindOpenMergeTabIndex(path);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  MakeCompareMergeService().OpenComparison(commit);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  return MakeCompareMergeService().OpenMergeEditor(base_path, incoming_path, current_path,
                                                   output_path);
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  return MakeCompareMergeService().OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool WorkspaceShell::OpenBranchHeadComparison(const std::filesystem::path& path,
                                              const std::string& left_ref,
                                              const std::string& left_label,
                                              const std::string& right_ref,
                                              const std::string& right_label) {
  return MakeCompareMergeService().OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                            right_label);
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  return MakeCompareMergeService().OpenGitConflictMerge(path);
}

void WorkspaceShell::OpenComparePicker() {
  MakeCompareMergeService().OpenPicker();
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  return MakeCompareMergeService().OpenPickerForPath(path, commit_spec);
}

void WorkspaceShell::RefreshComparePicker() {
  MakeCompareMergeService().RefreshPicker();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  MakeCompareMergeService().MovePickerSelection(delta);
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  MakeCompareMergeService().OpenSelectedCommit();
}

void WorkspaceShell::OpenWorkingFileFromCompare() {
  MakeCompareMergeService().OpenWorkingFileFromCompare();
}

void WorkspaceShell::OpenMergeResultFile() {
  MakeCompareMergeService().OpenMergeResultFile();
}

void WorkspaceShell::MoveCompareSelection(int delta) {
  MakeCompareMergeService().MoveCompareSelection(delta);
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  MakeCompareMergeService().JumpCompareHunk(delta);
}

void WorkspaceShell::ScrollCompareRows(int delta) {
  MakeCompareMergeService().ScrollCompareRows(delta);
}

void WorkspaceShell::ScrollCompareColumns(int delta) {
  MakeCompareMergeService().ScrollCompareColumns(delta);
}

void WorkspaceShell::MoveMergeSelection(int delta) {
  MakeCompareMergeService().MoveMergeSelection(delta);
}

void WorkspaceShell::ScrollMergeColumns(int delta) {
  MakeCompareMergeService().ScrollMergeColumns(delta);
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  MakeCompareMergeService().ApplyMergeChoice(choice);
}

}  // namespace microide::workspace
