#include "workspace/WorkspaceCompareInteractionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "project/GitCompareService.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

CompareInteractionCoordinator::CompareInteractionCoordinator(ProjectWorkspaceState& state,
                                                             Operations operations)
    : state_(state), operations_(std::move(operations)) {}

void CompareInteractionCoordinator::OpenPicker() {
  if (!state_.sidebar.visible || operations_.active_sidebar_mode() != SidebarMode::Tree) {
    return;
  }

  const auto& entries = state_.directory_tree.entries();
  if (state_.directory_tree.selected_index() >= entries.size()) {
    return;
  }

  const auto& entry = entries[state_.directory_tree.selected_index()];
  if (entry.is_directory) {
    return;
  }

  OpenPickerForPath(entry.path);
}

bool CompareInteractionCoordinator::OpenPickerForPath(
    const std::filesystem::path& path,
    std::string_view commit_spec) {
  if (state_.root.empty() || path.empty()) {
    return false;
  }

  state_.overlay.workflow.compare_picker.path = path.lexically_normal();
  state_.overlay.workflow.compare_picker.query.SetText("");
  state_.overlay.workflow.compare_picker.commits =
      project::CollectGitFileHistory(state_.root, state_.overlay.workflow.compare_picker.path);
  RefreshPicker();
  if (state_.overlay.workflow.compare_picker.matches.empty()) {
    return false;
  }

  if (!commit_spec.empty()) {
    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < state_.overlay.workflow.compare_picker.matches.size(); ++i) {
      const auto& commit = state_.overlay.workflow.compare_picker.matches[i];
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

    state_.overlay.workflow.compare_picker.selected_index = matching_indices.front();
    OpenSelectedCommit();
    return true;
  }

  operations_.show_compare_picker_overlay();
  return true;
}

void CompareInteractionCoordinator::RefreshPicker() {
  state_.overlay.workflow.compare_picker.matches.clear();
  state_.overlay.workflow.compare_picker.selected_index = 0;

  const std::string lowered_query = ToLower(state_.overlay.workflow.compare_picker.query.text());
  for (const auto& commit : state_.overlay.workflow.compare_picker.commits) {
    if (!lowered_query.empty()) {
      const std::string text = ToLower(commit.short_hash + " " + commit.subject);
      if (text.find(lowered_query) == std::string::npos) {
        continue;
      }
    }
    state_.overlay.workflow.compare_picker.matches.push_back(commit);
  }
  operations_.reset_overlay_scroll();
  operations_.request_overlay_redraw();
}

void CompareInteractionCoordinator::MovePickerSelection(int delta) {
  if (state_.overlay.workflow.compare_picker.matches.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(state_.overlay.workflow.compare_picker.selected_index);
  const int max_index =
      static_cast<int>(state_.overlay.workflow.compare_picker.matches.size()) - 1;
  state_.overlay.workflow.compare_picker.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (state_.overlay.visible) {
    operations_.reveal_compare_picker_selection();
  }
  operations_.request_overlay_redraw();
}

void CompareInteractionCoordinator::OpenSelectedCommit() {
  if (state_.overlay.workflow.compare_picker.matches.empty() ||
      state_.overlay.workflow.compare_picker.selected_index >=
          state_.overlay.workflow.compare_picker.matches.size()) {
    return;
  }

  operations_.open_comparison(
      state_.overlay.workflow.compare_picker.matches[state_.overlay.workflow.compare_picker.selected_index]);
}

void CompareInteractionCoordinator::OpenWorkingFileFromCompare() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
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

  operations_.open_file(compare_tab->path);
  if (target_line > 0) {
    if (editor::TextViewport* viewport = operations_.active_editor_viewport(); viewport != nullptr) {
      viewport->MoveCursorTo(static_cast<std::size_t>(target_line - 1), 0);
    }
  }
}

void CompareInteractionCoordinator::OpenMergeResultFile() {
  const MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->output_path.empty()) {
    return;
  }
  operations_.open_file(merge_tab->output_path);
}

void CompareInteractionCoordinator::MoveCompareSelection(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty() || delta == 0) {
    return;
  }

  const std::size_t previous_selected_row = compare_tab->selected_row;
  const int current = static_cast<int>(compare_tab->selected_row);
  const int max_index = static_cast<int>(compare_tab->model.rows.size()) - 1;
  compare_tab->selected_row =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  operations_.reveal_active_compare_selection();
  operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
  operations_.request_compare_row_range_redraw(compare_tab->selected_row, compare_tab->selected_row + 1);
}

void CompareInteractionCoordinator::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
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
  const std::size_t previous_selected_row = compare_tab->selected_row;
  target = std::clamp(target + delta, 0, static_cast<int>(compare_tab->model.hunks.size()) - 1);
  compare_tab->selected_row = static_cast<std::size_t>(
      compare_tab->model.hunks[static_cast<std::size_t>(target)].start_row);
  operations_.reveal_active_compare_selection();
  operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
  operations_.request_compare_row_range_redraw(compare_tab->selected_row, compare_tab->selected_row + 1);
}

void CompareInteractionCoordinator::ScrollCompareRows(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }
  operations_.scroll_compare_rows(*compare_tab, delta);
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::ScrollCompareColumns(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }
  operations_.scroll_compare_columns(*compare_tab, delta);
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::MoveMergeSelection(int delta) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty() || delta == 0) {
    return;
  }

  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  const int current = static_cast<int>(merge_tab->selected_hunk);
  const int max_index = static_cast<int>(merge_tab->conflicts.size()) - 1;
  merge_tab->selected_hunk =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  operations_.reveal_active_merge_selection();
  operations_.request_merge_conflict_redraw(previous_selected_hunk);
  operations_.request_merge_conflict_redraw(merge_tab->selected_hunk);
}

void CompareInteractionCoordinator::ScrollMergeColumns(int delta) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || delta == 0) {
    return;
  }
  operations_.scroll_merge_columns(*merge_tab, delta);
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::ApplyMergeChoice(compare::MergeChoice choice) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }

  const bool was_dirty = merge_tab->result_viewport.dirty();
  const std::size_t cursor_before_line = merge_tab->result_viewport.cursor_line();
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
  operations_.update_merge_max_visual_columns(*merge_tab, replacement_lines);
  merge_tab->hover_state.reset();
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  operations_.reveal_active_merge_selection();
  operations_.request_merge_result_line_to_bottom_redraw(conflict.start_line);
  if (merge_tab->result_viewport.dirty() != was_dirty) {
    operations_.request_active_editable_blame_neighborhood_redraw(
        cursor_before_line, merge_tab->result_viewport.cursor_line());
    operations_.request_tab_strip_redraw();
  }
}

}  // namespace microide::workspace
