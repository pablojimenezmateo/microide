#include "workspace/coordinators/WorkspaceCompareInteractionCoordinator.h"

#include "project/FileWriteGate.h"

#include <algorithm>

#include "project/GitStatusService.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResultValidation.h"
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "project/GitCompareService.h"
#include "project/PatchGenerator.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/SelectionMovement.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

GitPickerItem MakeCommitPickerItem(const project::GitCommitEntry& commit) {
  GitPickerItem item;
  item.kind = GitPickerItem::Kind::Commit;
  item.ref = commit.hash;
  item.apply_label = commit.short_hash;
  item.primary_label = commit.short_hash + "  " + commit.subject;
  if (!commit.author.empty() && !commit.relative_date.empty()) {
    item.secondary_label = commit.author + " · " + commit.relative_date;
  } else if (!commit.relative_date.empty()) {
    item.secondary_label = commit.relative_date;
  } else {
    item.secondary_label = commit.author;
  }
  item.search_text = ToLower(item.primary_label + " " + item.secondary_label + " " + item.ref);
  item.commit = commit;
  return item;
}

GitPickerItem MakeBranchPickerItem(const project::GitBranchReference& branch) {
  GitPickerItem item;
  item.kind = GitPickerItem::Kind::Branch;
  item.ref = branch.ref;
  item.apply_label = branch.label;
  item.primary_label = branch.label;
  item.secondary_label = "branch";
  item.search_text = ToLower(item.primary_label + " " + item.secondary_label + " " + item.ref);
  return item;
}

}

CompareInteractionCoordinator::CompareInteractionCoordinator(ProjectWorkspaceState& state,
                                                             Operations operations)
    : state_(state), operations_(std::move(operations)) {}

void CompareInteractionCoordinator::ApplyFileHistoryResult(
    const project::GitFileHistoryResult& history) {
  auto& picker = state_.overlay.workflow.compare_picker;
  picker.items.clear();
  picker.items.reserve(history.commits.size());
  for (const auto& commit : history.commits) {
    picker.items.push_back(MakeCommitPickerItem(commit));
  }
  picker.title = history.truncated ? "Compare against (latest 5000)" : "Compare against";
  picker.loading = false;
  RefreshPicker();
}

void CompareInteractionCoordinator::ApplyRefsResult(
    const std::vector<project::GitBranchReference>& branches,
    const std::vector<project::GitCommitEntry>& commits) {
  auto& picker = state_.overlay.workflow.compare_picker;
  picker.items.clear();

  if (picker.purpose == ComparePickerPurpose::SwitchBranch) {
    picker.items.reserve(branches.size());
    for (const auto& branch_ref : branches) {
      if (branch_ref.is_head) {
        continue;
      }
      GitPickerItem item = MakeBranchPickerItem(branch_ref);
      item.ref = branch_ref.label;
      item.secondary_label = branch_ref.is_remote ? "remote branch" : "branch";
      item.search_text = ToLower(item.primary_label + " " + item.secondary_label);
      picker.items.push_back(std::move(item));
    }
  } else {
    picker.items.reserve(branches.size() + commits.size());
    for (const auto& branch_ref : branches) {
      picker.items.push_back(MakeBranchPickerItem(branch_ref));
    }
    for (const auto& commit : commits) {
      picker.items.push_back(MakeCommitPickerItem(commit));
    }
  }
  picker.loading = false;
  RefreshPicker();
}

bool CompareInteractionCoordinator::OpenPickerForPath(
    const std::filesystem::path& path,
    std::string_view commit_spec) {
  if (state_.root.empty() || path.empty()) {
    return false;
  }

  auto& picker = state_.overlay.workflow.compare_picker;
  picker.purpose = ComparePickerPurpose::CompareFileHistory;
  picker.path = path.lexically_normal();
  picker.title = "Compare against";
  picker.context_label = picker.path.filename().string();
  picker.query.SetText("");
  picker.items.clear();
  picker.matches.clear();
  picker.selected_index = 0;

  if (!commit_spec.empty()) {
    picker.loading = false;
    const project::GitFileHistoryResult history =
        project::CollectGitFileHistory(state_.root, state_.launcher(), picker.path);
    ApplyFileHistoryResult(history);
    if (picker.matches.empty()) {
      return false;
    }

    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < picker.matches.size(); ++i) {
      const auto& item = picker.matches[i];
      if (item.kind != GitPickerItem::Kind::Commit) {
        continue;
      }
      const std::string lowered_hash = ToLower(item.commit.hash);
      const std::string lowered_short_hash = ToLower(item.commit.short_hash);
      if (StartsWith(lowered_hash, lowered_commit_spec) ||
          StartsWith(lowered_short_hash, lowered_commit_spec)) {
        matching_indices.push_back(i);
      }
    }

    if (matching_indices.size() != 1) {
      return false;
    }

    picker.selected_index = matching_indices.front();
    OpenSelectedCommit();
    return true;
  }

  picker.loading = true;
  RefreshPicker();
  util::InvokeIfSet(operations_.request_compare_file_history, picker.path);
  operations_.show_compare_picker_overlay();
  return true;
}

void CompareInteractionCoordinator::OpenOutgoingBasePicker() {
  if (state_.root.empty()) {
    return;
  }

  auto& picker = state_.overlay.workflow.compare_picker;
  picker.purpose = ComparePickerPurpose::OutgoingBaseRef;
  picker.path.clear();
  picker.title = "Choose outgoing base";
  const std::string& branch = state_.sidebar.git.branch_label;
  picker.context_label =
      branch.empty() ? std::string("Compare outgoing changes against…") : branch;
  picker.query.SetText("");
  picker.items.clear();
  picker.matches.clear();
  picker.selected_index = 0;

  picker.loading = true;
  RefreshPicker();
  util::InvokeIfSet(operations_.request_ref_list, true);
  operations_.show_compare_picker_overlay();
}

void CompareInteractionCoordinator::OpenBranchSwitchPicker() {
  if (state_.root.empty()) {
    return;
  }

  auto& picker = state_.overlay.workflow.compare_picker;
  picker.purpose = ComparePickerPurpose::SwitchBranch;
  picker.path.clear();
  picker.title = "Switch branch";
  const std::string& branch = state_.sidebar.git.branch_label;
  picker.context_label = branch.empty() ? std::string("Check out a branch") : ("on " + branch);
  picker.query.SetText("");
  picker.items.clear();
  picker.matches.clear();
  picker.selected_index = 0;

  picker.loading = true;
  RefreshPicker();
  util::InvokeIfSet(operations_.request_ref_list, false);
  operations_.show_compare_picker_overlay();
}

void CompareInteractionCoordinator::RefreshPicker() {
  auto& picker = state_.overlay.workflow.compare_picker;
  picker.matches.clear();
  picker.selected_index = 0;

  const std::string lowered_query = ToLower(picker.query.text());
  for (const auto& item : picker.items) {
    if (!lowered_query.empty() && item.search_text.find(lowered_query) == std::string::npos) {
      continue;
    }
    picker.matches.push_back(item);
  }
  picker.summary_line =
      BuildFilteredCountSummary(picker.matches.size(), picker.items.size(), "revisions");
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
  auto& picker = state_.overlay.workflow.compare_picker;
  if (picker.matches.empty() || picker.selected_index >= picker.matches.size()) {
    return;
  }

  const GitPickerItem& item = picker.matches[picker.selected_index];
  if (picker.purpose == ComparePickerPurpose::OutgoingBaseRef) {
    util::InvokeIfSet(operations_.set_outgoing_base_ref, item.ref, item.apply_label);
    return;
  }
  if (picker.purpose == ComparePickerPurpose::SwitchBranch) {
    util::InvokeIfSet(operations_.switch_to_branch, item.ref);
    return;
  }

  operations_.open_comparison(item.commit);
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
      viewport->JumpCursorTo(static_cast<std::size_t>(target_line - 1), 0);
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

  if (!MoveSelectionIndex(CompareTabPresentationRowCount(*compare_tab), &compare_tab->selected_row,
                          delta)) {
    return;
  }
  operations_.reveal_active_compare_selection();
  operations_.request_frame();
}

void CompareInteractionCoordinator::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || compare_tab->model.hunks.empty()) {
    return;
  }

  const int hunk_count = static_cast<int>(compare_tab->model.hunks.size());
  int target_hunk = -1;
  if (const int selected_hunk = CompareTabSelectedHunkIndex(*compare_tab); selected_hunk >= 0) {
    for (int i = 0; i < hunk_count; ++i) {
      if (compare_tab->model.hunks[static_cast<std::size_t>(i)].index == selected_hunk) {
        target_hunk = std::clamp(i + delta, 0, hunk_count - 1);
        break;
      }
    }
  }
  if (target_hunk < 0) {
    const auto model_row = static_cast<int>(CompareTabSelectedModelRow(*compare_tab));
    target_hunk = delta > 0 ? hunk_count - 1 : 0;
    if (delta > 0) {
      for (int i = 0; i < hunk_count; ++i) {
        if (compare_tab->model.hunks[static_cast<std::size_t>(i)].start_row > model_row) {
          target_hunk = i;
          break;
        }
      }
    } else {
      for (int i = hunk_count - 1; i >= 0; --i) {
        if (compare_tab->model.hunks[static_cast<std::size_t>(i)].start_row < model_row) {
          target_hunk = i;
          break;
        }
      }
    }
  }
  const int hunk_index = compare_tab->model.hunks[static_cast<std::size_t>(target_hunk)].index;
  if (const auto presentation_row = CompareTabPresentationRowForHunk(*compare_tab, hunk_index);
      presentation_row.has_value()) {
    compare_tab->selected_row = *presentation_row;
  } else {
    compare_tab->selected_row = compare::ComparePresentationRowForModelRow(
        compare_tab->presentation,
        static_cast<std::size_t>(
            compare_tab->model.hunks[static_cast<std::size_t>(target_hunk)].start_row));
  }
  SyncCompareCaretToSelectedRow(*compare_tab);
  operations_.reveal_active_compare_selection();
  operations_.request_frame();
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
  const std::vector<std::filesystem::path> review_files = compare_tab->review_files;
  const compare::CompareReviewMode review_mode = compare_tab->review_mode;
  const std::string commit_hash = compare_tab->commit_hash;
  const std::string left_label = compare_tab->left_label;
  const std::string right_ref = compare_tab->right_ref;
  const std::string right_label = compare_tab->right_label;
  const std::filesystem::path next_path =
      (state_.root / review_files[next_index]).lexically_normal();
  if (review_mode == compare::CompareReviewMode::Branch) {
    if (!operations_.open_branch_head_comparison(next_path, commit_hash, left_label, right_ref,
                                                 right_label, &review_files)) {
      return;
    }
  } else if (right_ref == "WORKTREE") {
    if (!operations_.open_working_tree_comparison(next_path, commit_hash, left_label)) {
      return;
    }
  } else {
    return;
  }
  if (CompareTabState* active = operations_.active_compare_tab(); active != nullptr) {
    active->review_file_index = next_index;
    active->review_files = review_files;
    active->review_mode = review_mode;
  }
}

void CompareInteractionCoordinator::CopyComparePath() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.write_clipboard_text) {
    return;
  }
  operations_.write_clipboard_text(compare_tab->path.lexically_normal().generic_string());
}

namespace {

std::filesystem::path RelativeToRootOrSelf(const std::filesystem::path& path,
                                           const std::filesystem::path& root) {
  if (!path.is_absolute() || root.empty()) {
    return path;
  }
  std::error_code error;
  std::filesystem::path relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    return path;
  }
  return relative;
}

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
  const std::filesystem::path relative = RelativeToRootOrSelf(compare_tab->path, state_.root);
  const std::optional<std::string> patch =
      project::GenerateComparePatch(compare_tab->model, relative, hunk_index);
  if (patch.has_value() && !patch->empty()) {
    operations_.write_clipboard_text(*patch);
  }
}

void CompareInteractionCoordinator::CopyCompareFilePatch() {
  const CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.write_clipboard_text) {
    return;
  }
  if (compare_tab->model.rows.empty()) {
    return;
  }
  const std::filesystem::path relative = RelativeToRootOrSelf(compare_tab->path, state_.root);
  const std::optional<std::string> patch = project::GenerateComparePatchForRows(
      compare_tab->model, relative, 0, compare_tab->model.rows.size() - 1);
  if (patch.has_value() && !patch->empty()) {
    operations_.write_clipboard_text(*patch);
  }
}

void CompareInteractionCoordinator::ToggleCompareIgnoreWhitespace() {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || !operations_.refresh_compare_tab_derived_state) {
    return;
  }
  compare_tab->build_options.ignore_whitespace = !compare_tab->build_options.ignore_whitespace;
  operations_.refresh_compare_tab_derived_state(*compare_tab);
  operations_.reveal_active_compare_selection();
  operations_.request_frame();
}

void CompareInteractionCoordinator::ToggleCompareShowWhitespace() {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr) {
    return;
  }
  compare_tab->show_whitespace = !compare_tab->show_whitespace;
  RefreshCompareTabPresentation(*compare_tab);
  operations_.request_frame();
}

void CompareInteractionCoordinator::ScrollCompareRows(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }
  operations_.scroll_compare_rows(*compare_tab, delta);
  operations_.request_frame();
}

void CompareInteractionCoordinator::ScrollCompareColumns(int delta) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab == nullptr || delta == 0) {
    return;
  }
  operations_.scroll_compare_columns(*compare_tab, delta);
  operations_.request_frame();
}

void CompareInteractionCoordinator::MoveMergeSelection(int delta) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr) {
    return;
  }

  if (!MoveSelectionIndex(merge_tab->conflicts.size(), &merge_tab->selected_hunk, delta)) {
    return;
  }
  operations_.reveal_active_merge_selection();
  operations_.request_frame();
}

void CompareInteractionCoordinator::ApplyMergeChoice(compare::MergeChoice choice) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty() ||
      !merge_tab->file_conflict.text_hunks_available) {
    return;
  }

  const bool was_dirty = merge_tab->result_viewport.dirty();
  const std::size_t selected_hunk =
      std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);
  auto& conflict = merge_tab->conflicts[selected_hunk];
  if (!conflict.valid || conflict.hunk_index >= merge_tab->model.hunks.size()) {
    return;
  }

  std::vector<std::string> replacement_lines =
      compare::MergeChoiceLines(merge_tab->model.hunks[conflict.hunk_index], choice);
  const std::size_t replacement_line_count = replacement_lines.size();
  const std::size_t previous_end = conflict.end_line;
  operations_.update_merge_max_visual_columns(*merge_tab, replacement_lines);
  if (!merge_tab->result_viewport.ReplaceLines(conflict.start_line, previous_end,
                                               std::move(replacement_lines)) &&
      merge_tab->result_viewport.read_only()) {
    return;
  }

  merge_tab->model.hunks[conflict.hunk_index].choice = choice;
  conflict.end_line = conflict.start_line + replacement_line_count;
  conflict.last_choice = choice;
  conflict.valid = true;
  conflict.resolved = true;
  merge_tab->marked_resolved = false;
  merge_tab->unresolved_override_prompt_pending = false;
  const long long line_delta =
      static_cast<long long>(conflict.end_line) - static_cast<long long>(previous_end);
  for (std::size_t i = selected_hunk + 1; i < merge_tab->conflicts.size(); ++i) {
    merge_tab->conflicts[i].start_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].start_line) + line_delta);
    merge_tab->conflicts[i].end_line = static_cast<std::size_t>(
        static_cast<long long>(merge_tab->conflicts[i].end_line) + line_delta);
  }
  merge_tab->hover_state.reset();
  merge_tab->scrollbar_marker_cache_valid = false;
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  operations_.reveal_active_merge_selection();
  operations_.request_frame();
  if (merge_tab->result_viewport.dirty() != was_dirty) {
    operations_.request_frame();
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
  merge_tab->unresolved_override_prompt_pending = false;
}

void CompareInteractionCoordinator::JumpNextUnresolvedMergeConflict() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->conflicts.empty()) {
    return;
  }
  const std::size_t start = merge_tab->selected_hunk;
  for (std::size_t offset = 1; offset <= merge_tab->conflicts.size(); ++offset) {
    const std::size_t index = (start + offset) % merge_tab->conflicts.size();
    if (merge_tab->conflicts[index].valid && !merge_tab->conflicts[index].resolved) {
      merge_tab->selected_hunk = index;
      operations_.reveal_active_merge_selection();
      operations_.request_frame();
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
  operations_.request_frame();
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
  const std::vector<std::string_view>& lines =
      incoming ? hunk.incoming_lines : hunk.current_lines;
  operations_.write_clipboard_text(
      util::SerializeLinesStreaming(lines, merge_tab->result_line_ending));
}

void CompareInteractionCoordinator::MarkMergeResolved() {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr || merge_tab->output_path.empty()) {
    return;
  }
  if (!merge_tab->file_conflict.text_hunks_available) {
    merge_tab->status_message = merge_tab->file_conflict.summary;
    operations_.request_frame();
    return;
  }

  const auto unresolved = static_cast<std::size_t>(
      std::count_if(merge_tab->conflicts.begin(), merge_tab->conflicts.end(),
                    [](const MergeTrackedConflict& conflict) { return conflict.valid && !conflict.resolved; }));
  if (unresolved > 0 && !merge_tab->unresolved_override_prompt_pending) {
    merge_tab->unresolved_override_prompt_pending = true;
    merge_tab->status_message =
        std::to_string(unresolved) + (unresolved == 1 ? " conflict is" : " conflicts are") +
        " still unresolved. Choose Mark Resolved again to keep the result as shown.";
    if (merge_tab->conflicts[std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1)].resolved) {
      JumpNextUnresolvedMergeConflict();
    }
    operations_.request_frame();
    return;
  }
  merge_tab->unresolved_override_prompt_pending = false;

  const bool result_should_exist = ResolvedResultShouldExist(*merge_tab);

  if (!result_should_exist) {
    const std::optional<std::string> backup = util::ReadTextFile(merge_tab->output_path);
    const bool prior_dirty = merge_tab->result_viewport.dirty();
    const std::optional<std::uint64_t> prior_disk_tick = merge_tab->disk_result_tick;
    const bool prior_external_stale = merge_tab->external_result_stale;

    const auto restore_working_file = [&]() {
      if (backup.has_value()) {
        (void)state_.write_gate().WriteText(merge_tab->output_path, *backup);
      }
      merge_tab->result_viewport.SetDirty(prior_dirty);
      merge_tab->disk_result_tick = prior_disk_tick;
      merge_tab->external_result_stale = prior_external_stale;
    };

    (void)state_.write_gate().ApplyTreeOp({.kind = project::FileWriteGate::TreeOp::Kind::Delete,
                                           .path = merge_tab->output_path});
    merge_tab->result_viewport.SetDirty(false);
    merge_tab->disk_result_tick = std::nullopt;
    merge_tab->external_result_stale = false;

    const MergeValidationResult delete_validation = ValidateMergeResult(MergeValidationRequest{
        .merge_tab = *merge_tab,
        .project_root = state_.root,
        .repository_generation = merge_tab->open_index_generation,
        .allow_conflict_marker_override = merge_tab->allow_conflict_marker_override,
        .result_should_exist = false,
    });
    if (!delete_validation.ok) {
      restore_working_file();
      merge_tab->status_message = delete_validation.message;
      merge_tab->index_stale =
          delete_validation.issue == MergeValidationIssue::StaleIndexGeneration;
      operations_.request_frame();
      return;
    }
    if (!operations_.stage_merge_result_path ||
        !operations_.stage_merge_result_path(merge_tab->output_path)) {
      restore_working_file();
      merge_tab->status_message = "Git could not mark the file resolved.";
      operations_.request_frame();
      return;
    }
    merge_tab->marked_resolved = true;
    merge_tab->marker_override_prompt_pending = false;
    merge_tab->allow_conflict_marker_override = false;
    merge_tab->status_message = "Marked resolved (deleted).";
    util::InvokeIfSet(operations_.refresh_git_sidebar);
    operations_.request_tab_strip_redraw();
    operations_.request_frame();
    return;
  }

  if (operations_.save_active_merge_tab && !operations_.save_active_merge_tab()) {
    merge_tab->status_message = "Could not save merge result.";
    operations_.request_frame();
    return;
  }
  if (!merge_tab->output_path.empty()) {
    merge_tab->disk_result_tick = util::FileModificationTick(merge_tab->output_path);
    merge_tab->external_result_stale = false;
  }

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
        merge_tab->result_viewport.JumpCursorTo(*validation.marker_line, 0);
      }
      operations_.request_frame();
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
    operations_.request_frame();
    return;
  }

  if (!operations_.stage_merge_result_path ||
      !operations_.stage_merge_result_path(merge_tab->output_path)) {
    merge_tab->status_message = "Git could not mark the file resolved.";
    operations_.request_frame();
    return;
  }

  merge_tab->marked_resolved = true;
  merge_tab->marker_override_prompt_pending = false;
  merge_tab->allow_conflict_marker_override = false;
  merge_tab->status_message = "Marked resolved.";
  util::InvokeIfSet(operations_.refresh_git_sidebar);
  operations_.request_tab_strip_redraw();
  operations_.request_frame();
}

}
