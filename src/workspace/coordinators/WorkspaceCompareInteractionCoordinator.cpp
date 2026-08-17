#include "workspace/coordinators/WorkspaceCompareInteractionCoordinator.h"

#include <algorithm>

#include "project/GitStatusService.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResultValidation.h"
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

namespace microide::workspace {

namespace {

// Builds a picker row for a commit. Display strings are composed here (not in the
// render TU) so painting only reads precomputed labels.
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

}  // namespace

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

void CompareInteractionCoordinator::ApplyFileHistoryResult(
    const project::GitFileHistoryResult& history) {
  auto& picker = state_.overlay.workflow.compare_picker;
  picker.items.clear();
  picker.items.reserve(history.commits.size());
  for (const auto& commit : history.commits) {
    picker.items.push_back(MakeCommitPickerItem(commit));
  }
  // Signal that older commits exist beyond the display cap rather than hiding the
  // truncation silently.
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
      // `git switch` takes the SHORT name for both local and remote-tracking
      // branches; the full ref would check out a detached HEAD for the latter.
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
    // Synchronous path (control channel / headless): the caller needs the bool
    // return and there is no UI to freeze. Run the git query inline.
    picker.loading = false;
    const project::GitFileHistoryResult history =
        project::CollectGitFileHistory(state_.root, picker.path);
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

  // Interactive path: open the overlay immediately in a loading state and let the
  // shell run CollectGitFileHistory on the background executor. The result lands
  // via ApplyFileHistoryResult on a later frame.
  picker.loading = true;
  RefreshPicker();
  if (operations_.request_compare_file_history) {
    operations_.request_compare_file_history(picker.path);
  }
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

  // Interactive-only: the branch + recent-commit queries run on the background
  // executor and populate via ApplyOutgoingBaseResult.
  picker.loading = true;
  RefreshPicker();
  if (operations_.request_ref_list) {
    operations_.request_ref_list(true);
  }
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
  if (operations_.request_ref_list) {
    operations_.request_ref_list(false);
  }
  operations_.show_compare_picker_overlay();
}

void CompareInteractionCoordinator::RefreshPicker() {
  auto& picker = state_.overlay.workflow.compare_picker;
  picker.matches.clear();
  picker.selected_index = 0;

  const std::string lowered_query = ToLower(picker.query.text());
  for (const auto& item : picker.items) {
    // item.search_text is the lowercased "primary secondary ref", precomputed once
    // when the item was built — avoids re-lowercasing + concatenating three strings
    // per item on every keystroke over a list that can hold thousands of commits.
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
    if (operations_.set_outgoing_base_ref) {
      operations_.set_outgoing_base_ref(item.ref, item.apply_label);
    }
    return;
  }
  if (picker.purpose == ComparePickerPurpose::SwitchBranch) {
    if (operations_.switch_to_branch) {
      operations_.switch_to_branch(item.ref);
    }
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
    // `selected_row` is a PRESENTATION row; a hunk's `start_row` is a model row,
    // and the two diverge the moment the diff collapses a run.
    compare_tab->selected_row = compare::ComparePresentationRowForModelRow(
        compare_tab->presentation,
        static_cast<std::size_t>(
            compare_tab->model.hunks[static_cast<std::size_t>(target_hunk)].start_row));
  }
  // The caret follows the change the jump revealed. Without this the reader looks
  // at hunk N and types into whatever line the caret was left on, which for a fresh
  // tab is line 0.
  SyncCompareCaretToSelectedRow(*compare_tab);
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
  // Opening the next comparison rebuilds the active compare tab (and can reallocate
  // open_tabs), so `compare_tab` may dangle afterward. Copy every value we still need
  // — for the reveal args and the post-open carry-over — into locals up front.
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
                                                 right_label)) {
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

// Relative-to-root label for a compare path, using the non-throwing
// std::filesystem::relative overload with a lexical fallback. The throwing
// overload could raise filesystem_error on the UI thread (deleted file, broken
// symlink, inaccessible mount) and terminate the app while merely copying a patch.
std::filesystem::path RelativeToRootOrSelf(const std::filesystem::path& path,
                                           const std::filesystem::path& root) {
  if (!path.is_absolute() || root.empty()) {
    return path;
  }
  std::error_code error;
  const std::filesystem::path relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    return path;
  }
  return relative;
}

}  // namespace

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
  // Route through the same real unified-diff generator the staging/discard path
  // uses, so a copied hunk is a genuine `git apply`-able patch (correct @@ line
  // ranges, /dev/null headers for add/delete, no-final-newline markers) rather
  // than a display-only approximation.
  const std::optional<std::string> patch =
      project::GenerateComparePatch(compare_tab->model, relative, hunk_index);
  // Only overwrite the clipboard when patch generation succeeded. Writing an
  // empty string on failure would silently destroy whatever the user had copied.
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
    // Nothing to copy; leave the user's existing clipboard intact.
    return;
  }
  const std::filesystem::path relative = RelativeToRootOrSelf(compare_tab->path, state_.root);
  // Whole-file patch: span every model row through the real generator so the
  // copied text is a genuine unified diff (same contract as the staging path),
  // not the fake `@@ hunk N @@` headers the display-only exporter emitted.
  const std::optional<std::string> patch = project::GenerateComparePatchForRows(
      compare_tab->model, relative, 0, compare_tab->model.rows.size() - 1);
  // Only overwrite the clipboard on success — do not clobber it with an empty
  // string when patch generation fails.
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

  std::vector<std::string> replacement_lines =
      compare::MergeChoiceLines(merge_tab->model.hunks[conflict.hunk_index], choice);
  const std::size_t replacement_line_count = replacement_lines.size();
  const std::size_t previous_end = conflict.end_line;
  if (!merge_tab->result_viewport.ReplaceLines(conflict.start_line, previous_end,
                                               std::move(replacement_lines))) {
    return;
  }

  merge_tab->model.hunks[conflict.hunk_index].choice = choice;
  conflict.end_line = conflict.start_line + replacement_line_count;
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
  // Accepting a side rewrote this conflict's span and shifted every following one,
  // so the cached overview-ruler markers (keyed on model_revision) are stale.
  // Invalidate them or the scrollbar keeps pointing markers at pre-accept rows/colors.
  merge_tab->scrollbar_marker_cache_valid = false;
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
  // Streaming form: a hunk's lines are views into the model's source buffers, and
  // this serializes straight out of them rather than materializing owned copies
  // first.
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
    operations_.request_editor_surface_redraw();
    return;
  }

  const bool result_should_exist = ResolvedResultShouldExist(*merge_tab);

  // Delete resolution: a modify/delete-class conflict reduced to an empty result
  // means the user accepted the deletion. Remove the working file and stage that
  // removal instead of writing (and staging) an empty file. This is the only way
  // such a conflict can naturally resolve to "deleted".
  if (!result_should_exist) {
    // Transactional deletion: removing the working file is irreversible, so capture
    // the on-disk bytes first and roll them back if validation or staging fails.
    // Otherwise a stage failure (stale index, git error, permissions) would leave the
    // user's in-progress merge file gone with no way to recover it.
    const std::optional<std::string> backup = util::ReadTextFile(merge_tab->output_path);
    const bool prior_dirty = merge_tab->result_viewport.dirty();
    const std::optional<std::uint64_t> prior_disk_tick = merge_tab->disk_result_tick;
    const bool prior_external_stale = merge_tab->external_result_stale;

    const auto restore_working_file = [&]() {
      if (backup.has_value()) {
        util::WriteTextFileAtomically(merge_tab->output_path, *backup);
      }
      merge_tab->result_viewport.SetDirty(prior_dirty);
      merge_tab->disk_result_tick = prior_disk_tick;
      merge_tab->external_result_stale = prior_external_stale;
    };

    std::error_code remove_error;
    std::filesystem::remove(merge_tab->output_path, remove_error);
    // The file is gone; the buffer is no longer the source of truth. Mark it clean
    // so a later Save cannot resurrect the file, and drop the disk tick / stale flag.
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
      operations_.request_editor_surface_redraw();
      return;
    }
    if (!operations_.stage_merge_result_path ||
        !operations_.stage_merge_result_path(merge_tab->output_path)) {
      restore_working_file();
      merge_tab->status_message = "Git could not mark the file resolved.";
      operations_.request_editor_surface_redraw();
      return;
    }
    merge_tab->marked_resolved = true;
    merge_tab->marker_override_prompt_pending = false;
    merge_tab->allow_conflict_marker_override = false;
    merge_tab->status_message = "Marked resolved (deleted).";
    if (operations_.refresh_git_sidebar) {
      operations_.refresh_git_sidebar();
    }
    operations_.request_tab_strip_redraw();
    operations_.request_editor_surface_redraw();
    return;
  }

  if (operations_.save_active_merge_tab && !operations_.save_active_merge_tab()) {
    merge_tab->status_message = "Could not save merge result.";
    operations_.request_editor_surface_redraw();
    return;
  }
  // The save above just rewrote output_path — TextViewport::Save always does an
  // atomic write+rename, which bumps the mtime even when the buffer was clean. Sync
  // disk_result_tick to that fresh value (via the same function ValidateMergeResult
  // reads) so our own write is not misread as an external modification, which would
  // otherwise reject every Mark Resolved and never stage the file.
  if (!merge_tab->output_path.empty()) {
    merge_tab->disk_result_tick = FileModificationTick(merge_tab->output_path);
    // We have just reconciled disk_result_tick to our own write, so any prior
    // "changed on disk" flag is stale — clearing it is required, otherwise the
    // boolean guard in ValidateMergeResult short-circuits before the (now matching)
    // tick comparison and rejects every Mark Resolved for the life of the tab.
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
