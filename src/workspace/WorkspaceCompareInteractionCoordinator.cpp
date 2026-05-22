#include "workspace/WorkspaceCompareInteractionCoordinator.h"

#include <algorithm>

#include "project/GitStatusService.h"
#include "util/TextFileIO.h"
#include "workspace/MergeResultValidation.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compare/ComparePatchExport.h"
#include "project/GitCompareService.h"
#include "workspace/CompareTabReview.h"
#include "workspace/SelectionMovement.h"
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
  if (!MoveSelectionIndex(state_.overlay.workflow.compare_picker.matches.size(),
                          &state_.overlay.workflow.compare_picker.selected_index, delta)) {
    return;
  }

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

  const auto& row = CompareTabSelectedModelRowRef(*compare_tab);
  int target_line = row.right_line;
  if (target_line == 0) {
    const std::size_t model_row = CompareTabSelectedModelRow(*compare_tab);
    for (std::size_t i = model_row + 1; i < compare_tab->model.rows.size(); ++i) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }
  if (target_line == 0) {
    const std::size_t model_row = CompareTabSelectedModelRow(*compare_tab);
    for (std::size_t i = model_row; i-- > 0;) {
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
  if (compare_tab == nullptr) {
    return;
  }

  const std::size_t previous_selected_row = compare_tab->selected_row;
  if (!MoveSelectionIndex(CompareTabPresentationRowCount(*compare_tab), &compare_tab->selected_row,
                          delta)) {
    return;
  }
  operations_.reveal_active_compare_selection();
  operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
  operations_.request_compare_row_range_redraw(compare_tab->selected_row, compare_tab->selected_row + 1);
}

void CompareInteractionCoordinator::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || compare_tab->model.hunks.empty()) {
    return;
  }

  int target_hunk = CompareTabSelectedHunkIndex(*compare_tab);
  if (target_hunk < 0) {
    target_hunk = 0;
  } else {
    for (std::size_t i = 0; i < compare_tab->model.hunks.size(); ++i) {
      const int hunk_index = compare_tab->model.hunks[i].index;
      if (hunk_index == target_hunk) {
        target_hunk = static_cast<int>(i);
        break;
      }
    }
  }
  const std::size_t previous_selected_row = compare_tab->selected_row;
  target_hunk = std::clamp(target_hunk + delta, 0,
                           static_cast<int>(compare_tab->model.hunks.size()) - 1);
  const int hunk_index = compare_tab->model.hunks[static_cast<std::size_t>(target_hunk)].index;
  if (const auto presentation_row = CompareTabPresentationRowForHunk(*compare_tab, hunk_index);
      presentation_row.has_value()) {
    compare_tab->selected_row = *presentation_row;
  } else {
    compare_tab->selected_row = static_cast<std::size_t>(
        compare_tab->model.hunks[static_cast<std::size_t>(target_hunk)].start_row);
  }
  operations_.reveal_active_compare_selection();
  operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
  operations_.request_compare_row_range_redraw(compare_tab->selected_row, compare_tab->selected_row + 1);
}

void CompareInteractionCoordinator::JumpCompareReviewFile(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || compare_tab->review_files.size() < 2 || delta == 0) {
    return;
  }

  std::size_t next_index = compare_tab->review_file_index;
  if (!MoveSelectionIndex(compare_tab->review_files.size(), &next_index, delta)) {
    return;
  }
  const std::filesystem::path next_path =
      (state_.root / compare_tab->review_files[next_index]).lexically_normal();
  if (compare_tab->review_mode == compare::CompareReviewMode::Branch) {
    if (!operations_.open_branch_head_comparison(
            next_path, compare_tab->commit_hash, compare_tab->left_label, compare_tab->right_ref,
            compare_tab->right_label)) {
      return;
    }
  } else if (compare_tab->right_ref == "WORKTREE") {
    if (!operations_.open_working_tree_comparison(next_path, compare_tab->commit_hash,
                                                  compare_tab->left_label)) {
      return;
    }
  } else {
    return;
  }
  if (CompareTabState* active = operations_.active_compare_tab(); active != nullptr) {
    active->review_file_index = next_index;
    active->review_files = compare_tab->review_files;
    active->review_mode = compare_tab->review_mode;
  }
}

void CompareInteractionCoordinator::CopyComparePath() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.write_clipboard_text) {
    return;
  }
  operations_.write_clipboard_text(compare_tab->path.lexically_normal().generic_string());
}

void CompareInteractionCoordinator::CopyCompareHunkPatch() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.write_clipboard_text) {
    return;
  }
  const int hunk_index = CompareTabSelectedHunkIndex(*compare_tab);
  if (hunk_index < 0) {
    return;
  }
  const std::filesystem::path relative =
      compare_tab->path.is_absolute() && !state_.root.empty()
          ? std::filesystem::relative(compare_tab->path, state_.root)
          : compare_tab->path;
  operations_.write_clipboard_text(
      compare::FormatCompareHunkPatch(compare_tab->model, hunk_index, relative));
}

void CompareInteractionCoordinator::CopyCompareFilePatch() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.write_clipboard_text) {
    return;
  }
  const std::filesystem::path relative =
      compare_tab->path.is_absolute() && !state_.root.empty()
          ? std::filesystem::relative(compare_tab->path, state_.root)
          : compare_tab->path;
  operations_.write_clipboard_text(
      compare::FormatCompareFilePatch(compare_tab->model, relative));
}

void CompareInteractionCoordinator::ToggleCompareIgnoreWhitespace() {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.refresh_compare_tab_derived_state) {
    return;
  }
  compare_tab->build_options.ignore_whitespace = !compare_tab->build_options.ignore_whitespace;
  operations_.refresh_compare_tab_derived_state(*compare_tab);
  operations_.reveal_active_compare_selection();
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::ToggleCompareShowWhitespace() {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr) {
    return;
  }
  compare_tab->show_whitespace = !compare_tab->show_whitespace;
  RefreshCompareTabPresentation(*compare_tab);
  operations_.request_editor_surface_redraw();
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
  if (merge_tab == nullptr) {
    return;
  }

  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  if (!MoveSelectionIndex(merge_tab->conflicts.size(), &merge_tab->selected_hunk, delta)) {
    return;
  }
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
  if (merge_tab == nullptr || merge_tab->conflicts.empty() ||
      !merge_tab->file_conflict.text_hunks_available) {
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
  conflict.resolved = true;
  merge_tab->marked_resolved = false;
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

void CompareInteractionCoordinator::ResetMergeHunk() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }
  const std::size_t selected_hunk =
      std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);
  auto& conflict = merge_tab->conflicts[selected_hunk];
  if (!conflict.valid || conflict.hunk_index >= merge_tab->model.hunks.size()) {
    return;
  }
  auto& hunk = merge_tab->model.hunks[conflict.hunk_index];
  hunk.choice = conflict.bootstrap_choice;
  ApplyMergeChoice(conflict.bootstrap_choice);
  conflict.resolved = false;
  merge_tab->marked_resolved = false;
}

void CompareInteractionCoordinator::JumpNextUnresolvedMergeConflict() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }
  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  const std::size_t start = merge_tab->selected_hunk;
  for (std::size_t offset = 1; offset <= merge_tab->conflicts.size(); ++offset) {
    const std::size_t index = (start + offset) % merge_tab->conflicts.size();
    if (merge_tab->conflicts[index].valid && !merge_tab->conflicts[index].resolved) {
      merge_tab->selected_hunk = index;
      operations_.reveal_active_merge_selection();
      operations_.request_merge_conflict_redraw(previous_selected_hunk);
      operations_.request_merge_conflict_redraw(index);
      return;
    }
  }
}

void CompareInteractionCoordinator::ToggleMergeBasePane() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr) {
    return;
  }
  merge_tab->base_pane_visible = !merge_tab->base_pane_visible;
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::ToggleMergeRawMarkers() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr) {
    return;
  }
  merge_tab->show_raw_markers = !merge_tab->show_raw_markers;
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::CopyMergeSideSnippet(bool incoming) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty() || !operations_.write_clipboard_text) {
    return;
  }
  const auto& conflict =
      merge_tab->conflicts[std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1)];
  if (!conflict.valid || conflict.hunk_index >= merge_tab->model.hunks.size()) {
    return;
  }
  const auto& hunk = merge_tab->model.hunks[conflict.hunk_index];
  const std::vector<std::string>& lines =
      incoming ? hunk.incoming_lines : hunk.current_lines;
  operations_.write_clipboard_text(util::SerializeLines(lines, merge_tab->result_line_ending));
}

void CompareInteractionCoordinator::MarkMergeResolved() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->output_path.empty()) {
    return;
  }
  if (!merge_tab->file_conflict.text_hunks_available) {
    merge_tab->status_message = merge_tab->file_conflict.summary;
    operations_.request_editor_surface_redraw();
    return;
  }
  if (operations_.save_active_merge_tab && !operations_.save_active_merge_tab()) {
    merge_tab->status_message = "Could not save merge result.";
    operations_.request_editor_surface_redraw();
    return;
  }

  const bool result_should_exist = !merge_tab->file_conflict.requires_existence_choice ||
                                   !merge_tab->result_viewport.lines().empty();
  MergeValidationRequest request{
      .merge_tab = *merge_tab,
      .project_root = state_.root,
      .repository_generation = merge_tab->open_index_generation,
      .allow_conflict_marker_override = merge_tab->allow_conflict_marker_override,
      .result_should_exist = result_should_exist,
  };
  MergeValidationResult validation = ValidateMergeResult(request);
  if (!validation.ok && validation.issue == MergeValidationIssue::ConflictMarkers &&
      !merge_tab->allow_conflict_marker_override) {
    if (merge_tab->marker_override_prompt_pending) {
      merge_tab->allow_conflict_marker_override = true;
      merge_tab->marker_override_prompt_pending = false;
      request.allow_conflict_marker_override = true;
      validation = ValidateMergeResult(request);
    } else {
      merge_tab->marker_override_prompt_pending = true;
      merge_tab->status_message =
          validation.message + " Choose Mark Resolved again to override.";
      if (validation.marker_line.has_value()) {
        merge_tab->result_viewport.MoveCursorTo(*validation.marker_line, 0);
      }
      operations_.request_editor_surface_redraw();
      return;
    }
  }
  if (!validation.ok) {
    merge_tab->status_message = validation.message;
    if (validation.issue == MergeValidationIssue::StaleIndexGeneration ||
        validation.issue == MergeValidationIssue::ExternalModification) {
      merge_tab->index_stale = validation.issue == MergeValidationIssue::StaleIndexGeneration;
      merge_tab->external_result_stale =
          validation.issue == MergeValidationIssue::ExternalModification;
    }
    operations_.request_editor_surface_redraw();
    return;
  }

  if (!operations_.stage_merge_result_path ||
      !operations_.stage_merge_result_path(merge_tab->output_path)) {
    merge_tab->status_message = "Git could not mark the file resolved.";
    operations_.request_editor_surface_redraw();
    return;
  }

  merge_tab->marked_resolved = true;
  merge_tab->marker_override_prompt_pending = false;
  merge_tab->allow_conflict_marker_override = false;
  merge_tab->status_message = "Marked resolved.";
  if (operations_.refresh_git_sidebar) {
    operations_.refresh_git_sidebar();
  }
  operations_.request_tab_strip_redraw();
  operations_.request_editor_surface_redraw();
}

void CompareInteractionCoordinator::StageCompareHunk() {
  if (operations_.stage_compare_hunk) {
    operations_.stage_compare_hunk();
  }
}

void CompareInteractionCoordinator::StageCompareSelectedLines() {
  if (operations_.stage_compare_selected_lines) {
    operations_.stage_compare_selected_lines();
  }
}

void CompareInteractionCoordinator::UnstageCompareHunk() {
  if (operations_.unstage_compare_hunk) {
    operations_.unstage_compare_hunk();
  }
}

void CompareInteractionCoordinator::UnstageCompareSelectedLines() {
  if (operations_.unstage_compare_selected_lines) {
    operations_.unstage_compare_selected_lines();
  }
}

void CompareInteractionCoordinator::OpenDiscardCompareHunkPrompt() {
  if (operations_.open_discard_compare_hunk_prompt) {
    operations_.open_discard_compare_hunk_prompt();
  }
}

void CompareInteractionCoordinator::OpenDiscardCompareSelectedLinesPrompt() {
  if (operations_.open_discard_compare_selected_lines_prompt) {
    operations_.open_discard_compare_selected_lines_prompt();
  }
}

}  // namespace microide::workspace
