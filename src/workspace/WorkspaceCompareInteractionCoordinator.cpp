#include "workspace/WorkspaceCompareInteractionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "project/GitCompareService.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::CompareInteractionCoordinator::CompareInteractionCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

void WorkspaceShell::CompareInteractionCoordinator::OpenPicker() {
  if (!shell_.surface_.sidebar_visible || shell_.surface_.sidebar_mode != SidebarMode::Tree) {
    return;
  }

  const auto& entries = shell_.directory_tree_.entries();
  if (shell_.directory_tree_.selected_index() >= entries.size()) {
    return;
  }

  const auto& entry = entries[shell_.directory_tree_.selected_index()];
  if (entry.is_directory) {
    return;
  }

  OpenPickerForPath(entry.path);
}

bool WorkspaceShell::CompareInteractionCoordinator::OpenPickerForPath(
    const std::filesystem::path& path,
    std::string_view commit_spec) {
  if (shell_.project_root_.empty() || path.empty()) {
    return false;
  }

  shell_.overlay_workflow_.compare_picker.path = path.lexically_normal();
  shell_.overlay_workflow_.compare_picker.query.clear();
  shell_.overlay_workflow_.compare_picker.commits =
      project::CollectGitFileHistory(shell_.project_root_, shell_.overlay_workflow_.compare_picker.path);
  RefreshPicker();
  if (shell_.overlay_workflow_.compare_picker.matches.empty()) {
    return false;
  }

  if (!commit_spec.empty()) {
    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < shell_.overlay_workflow_.compare_picker.matches.size(); ++i) {
      const auto& commit = shell_.overlay_workflow_.compare_picker.matches[i];
      const std::string lowered_hash = ToLower(commit.hash);
      const std::string lowered_short_hash = ToLower(commit.short_hash);
      if (StartsWith(lowered_hash, lowered_commit_spec) ||
          StartsWith(lowered_short_hash, lowered_commit_spec)) {
        matching_indices.push_back(i);
      }
    }

    if (matching_indices.size() != 1) {
      return false;
    }

    shell_.overlay_workflow_.compare_picker.selected_index = matching_indices.front();
    OpenSelectedCommit();
    return true;
  }

  shell_.ShowOverlay(OverlayMode::CommitPicker);
  return true;
}

void WorkspaceShell::CompareInteractionCoordinator::RefreshPicker() {
  shell_.overlay_workflow_.compare_picker.matches.clear();
  shell_.overlay_workflow_.compare_picker.selected_index = 0;

  const std::string lowered_query = ToLower(shell_.overlay_workflow_.compare_picker.query);
  for (const auto& commit : shell_.overlay_workflow_.compare_picker.commits) {
    if (!lowered_query.empty()) {
      const std::string text = ToLower(commit.short_hash + " " + commit.subject);
      if (text.find(lowered_query) == std::string::npos) {
        continue;
      }
    }
    shell_.overlay_workflow_.compare_picker.matches.push_back(commit);
  }
  shell_.ResetOverlayScroll();
}

void WorkspaceShell::CompareInteractionCoordinator::MovePickerSelection(int delta) {
  if (shell_.overlay_workflow_.compare_picker.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(shell_.overlay_workflow_.compare_picker.selected_index);
  const int max_index =
      static_cast<int>(shell_.overlay_workflow_.compare_picker.matches.size()) - 1;
  shell_.overlay_workflow_.compare_picker.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (shell_.surface_.overlay_visible) {
    if (const auto layout = shell_.CurrentWorkspaceLayout(); layout.has_value()) {
      shell_.RevealOverlaySelection(shell_.ComputeOverlayRect(layout->editor_area));
    }
  }
}

void WorkspaceShell::CompareInteractionCoordinator::OpenSelectedCommit() {
  if (shell_.overlay_workflow_.compare_picker.matches.empty() ||
      shell_.overlay_workflow_.compare_picker.selected_index >=
          shell_.overlay_workflow_.compare_picker.matches.size()) {
    return;
  }

  shell_.OpenComparison(
      shell_.overlay_workflow_.compare_picker.matches[shell_.overlay_workflow_.compare_picker.selected_index]);
}

void WorkspaceShell::CompareInteractionCoordinator::OpenWorkingFileFromCompare() {
  const CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty()) {
    return;
  }

  const auto& row = compare_tab->model.rows[compare_tab->selected_row];
  int target_line = row.right_line;
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row + 1; i < compare_tab->model.rows.size(); ++i) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row; i-- > 0;) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }

  shell_.OpenFile(compare_tab->path);
  if (target_line > 0) {
    shell_.text_viewport_.MoveCursorTo(static_cast<std::size_t>(target_line - 1), 0);
  }
}

void WorkspaceShell::CompareInteractionCoordinator::OpenMergeResultFile() {
  const MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->output_path.empty()) {
    return;
  }
  shell_.OpenFile(merge_tab->output_path);
}

void WorkspaceShell::CompareInteractionCoordinator::MoveCompareSelection(int delta) {
  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(compare_tab->selected_row);
  const int max_index = static_cast<int>(compare_tab->model.rows.size()) - 1;
  compare_tab->selected_row =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  shell_.RevealActiveCompareSelection();
}

void WorkspaceShell::CompareInteractionCoordinator::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.hunks.empty()) {
    return;
  }

  int target = 0;
  for (std::size_t i = 0; i < compare_tab->model.hunks.size(); ++i) {
    if (compare_tab->model.hunks[i].start_row >= static_cast<int>(compare_tab->selected_row)) {
      target = static_cast<int>(i);
      break;
    }
    target = static_cast<int>(i);
  }
  target = std::clamp(target + delta, 0, static_cast<int>(compare_tab->model.hunks.size()) - 1);
  compare_tab->selected_row = static_cast<std::size_t>(
      compare_tab->model.hunks[static_cast<std::size_t>(target)].start_row);
  shell_.RevealActiveCompareSelection();
}

void WorkspaceShell::CompareInteractionCoordinator::ScrollCompareRows(int delta) {
  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  const CompareSurfaceLayout surface_layout =
      shell_.ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      shell_.ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row =
      std::clamp(scroll_layout.vertical_scroll + delta, 0, scroll_layout.max_vertical_scroll);
  shell_.SyncCompareViewportScroll(*compare_tab);
}

void WorkspaceShell::CompareInteractionCoordinator::ScrollCompareColumns(int delta) {
  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  const CompareSurfaceLayout surface_layout =
      shell_.ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      shell_.ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  const long long target_scroll =
      static_cast<long long>(scroll_layout.horizontal_scroll) + static_cast<long long>(delta);
  compare_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL,
                 static_cast<long long>(scroll_layout.max_horizontal_scroll)));
  shell_.SyncCompareViewportScroll(*compare_tab);
}

void WorkspaceShell::CompareInteractionCoordinator::MoveMergeSelection(int delta) {
  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(merge_tab->selected_hunk);
  const int max_index = static_cast<int>(merge_tab->conflicts.size()) - 1;
  merge_tab->selected_hunk =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  shell_.RevealActiveMergeSelection();
}

void WorkspaceShell::CompareInteractionCoordinator::ScrollMergeColumns(int delta) {
  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr || delta == 0) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  const MergeSurfaceLayout surface_layout =
      shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      shell_.ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
  const long long target_scroll =
      static_cast<long long>(scroll_layout.horizontal_scroll) + static_cast<long long>(delta);
  merge_tab->horizontal_scroll = static_cast<std::size_t>(
      std::clamp(target_scroll, 0LL,
                 static_cast<long long>(scroll_layout.max_horizontal_scroll)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
}

void WorkspaceShell::CompareInteractionCoordinator::ApplyMergeChoice(compare::MergeChoice choice) {
  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }

  const std::size_t selected_hunk =
      std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);
  auto& conflict = merge_tab->conflicts[selected_hunk];
  if (!conflict.valid || conflict.hunk_index >= merge_tab->model.hunks.size()) {
    return;
  }

  const std::vector<std::string> replacement_lines =
      compare::MergeChoiceLines(merge_tab->model.hunks[conflict.hunk_index], choice);
  const std::size_t previous_end = conflict.end_line;
  if (!merge_tab->result_viewport.ReplaceLines(conflict.start_line, previous_end,
                                               replacement_lines)) {
    return;
  }

  merge_tab->model.hunks[conflict.hunk_index].choice = choice;
  conflict.end_line = conflict.start_line + replacement_lines.size();
  conflict.last_choice = choice;
  conflict.valid = true;
  const long long line_delta =
      static_cast<long long>(conflict.end_line) - static_cast<long long>(previous_end);
  for (std::size_t i = selected_hunk + 1; i < merge_tab->conflicts.size(); ++i) {
    merge_tab->conflicts[i].start_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].start_line) + line_delta);
    merge_tab->conflicts[i].end_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].end_line) + line_delta);
  }
  shell_.UpdateMergeMaxVisualColumns(*merge_tab, replacement_lines);
  merge_tab->hover_state.reset();
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  shell_.RevealActiveMergeSelection();
}

void WorkspaceShell::OpenComparePicker() {
  CompareInteractionCoordinator(*this).OpenPicker();
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  return CompareInteractionCoordinator(*this).OpenPickerForPath(path, commit_spec);
}

void WorkspaceShell::RefreshComparePicker() {
  CompareInteractionCoordinator(*this).RefreshPicker();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  CompareInteractionCoordinator(*this).MovePickerSelection(delta);
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  CompareInteractionCoordinator(*this).OpenSelectedCommit();
}

void WorkspaceShell::OpenWorkingFileFromCompare() {
  CompareInteractionCoordinator(*this).OpenWorkingFileFromCompare();
}

void WorkspaceShell::OpenMergeResultFile() {
  CompareInteractionCoordinator(*this).OpenMergeResultFile();
}

void WorkspaceShell::MoveCompareSelection(int delta) {
  CompareInteractionCoordinator(*this).MoveCompareSelection(delta);
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  CompareInteractionCoordinator(*this).JumpCompareHunk(delta);
}

void WorkspaceShell::ScrollCompareRows(int delta) {
  CompareInteractionCoordinator(*this).ScrollCompareRows(delta);
}

void WorkspaceShell::ScrollCompareColumns(int delta) {
  CompareInteractionCoordinator(*this).ScrollCompareColumns(delta);
}

void WorkspaceShell::MoveMergeSelection(int delta) {
  CompareInteractionCoordinator(*this).MoveMergeSelection(delta);
}

void WorkspaceShell::ScrollMergeColumns(int delta) {
  CompareInteractionCoordinator(*this).ScrollMergeColumns(delta);
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  CompareInteractionCoordinator(*this).ApplyMergeChoice(choice);
}

}  // namespace microide::workspace
