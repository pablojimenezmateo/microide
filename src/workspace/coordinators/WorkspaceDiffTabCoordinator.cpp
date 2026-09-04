#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "compare/CompareReviewTypes.h"
#include "project/GitCompareService.h"
#include "util/TextFileIO.h"
#include "workspace/git/CompareTabReview.h"

namespace microide::workspace {

namespace {

void NotifyBufferOpenForEditableTab(const TabEntry& tab,
                                    const DiffTabCoordinator::Operations& operations) {
  if (!operations.notify_plugin_buffer_open) {
    return;
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
      tab.compare->right_editable && !tab.compare->right_viewport.path().empty()) {
    operations.notify_plugin_buffer_open(tab.compare->right_viewport.path());
    return;
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
      !tab.merge->result_viewport.path().empty()) {
    operations.notify_plugin_buffer_open(tab.merge->result_viewport.path());
  }
}

// A freshly-opened comparison lands on its FIRST change, with the editable pane's
// caret on it — what VS Code's diff editor does, and what the reader opened the
// diff for. Row 0 is whatever unchanged context precedes the first hunk, which on a
// large file is the entire screen.
//
// Fresh tabs only. Re-activating an already-open comparison goes through
// ActivateCompareTab without this, because the reader's own selection is the thing
// to preserve there.
void SelectFirstChangeOnOpen(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
    return;
  }
  CompareTabState& compare_tab = *tab.compare;
  compare_tab.selected_row = CompareFirstChangePresentationRow(compare_tab);
  NormalizeCompareSelectionToModelRow(compare_tab);
  SyncCompareCaretToSelectedRow(compare_tab);
}

}  // namespace

std::optional<CompareInput> ReadFileCompareInput(const std::filesystem::path& path, bool editable) {
  const std::filesystem::path normalized = util::NormalizedPath(path);
  const util::TextFileReadResult read = util::ReadTextFileClassified(normalized);
  if (read.is_error()) {
    return std::nullopt;
  }
  return CompareInput{
      .content = read.ok() ? read.content : std::string{},  // missing file -> empty (deleted) side
      .label = normalized.filename().string(),
      .path = normalized,
      .editable = editable,
  };
}

DiffTabCoordinator::DiffTabCoordinator(ProjectWorkspaceState& state, Operations operations)
    : state_(state), operations_(std::move(operations)) {}

std::optional<std::size_t> DiffTabCoordinator::FindOpenCompareTabIndex(
    const std::filesystem::path& path, std::string_view left_ref, std::string_view right_ref) const {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    const auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (tab.compare->path == normalized_path && tab.compare->commit_hash == left_ref &&
        tab.compare->right_ref == right_ref) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> DiffTabCoordinator::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    const auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }
    if (tab.merge->output_path == normalized_path) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> DiffTabCoordinator::FindOpenPlainCompareTabIndex(
    const std::filesystem::path& left_path, const std::filesystem::path& right_path) const {
  const std::filesystem::path normalized_left = util::NormalizedPath(left_path);
  const std::filesystem::path normalized_right = util::NormalizedPath(right_path);
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    const auto& tab = state_.focused_group().open_tabs[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
        !tab.compare->plain_compare) {
      continue;
    }
    if (tab.compare->left_path == normalized_left && tab.compare->right_path == normalized_right) {
      return i;
    }
  }
  return std::nullopt;
}

bool DiffTabCoordinator::OpenPlainComparison(CompareInput left, CompareInput right) {
  const std::filesystem::path left_path = util::NormalizedPath(left.path);
  const std::filesystem::path right_path = util::NormalizedPath(right.path);
  // Dedup only when both sides are real files. A clipboard/untitled side has no
  // stable identity and its content may have changed, so re-running always opens
  // a fresh tab reflecting the caller's freshly-resolved content.
  if (!left_path.empty() && !right_path.empty()) {
    if (const auto existing_index = FindOpenPlainCompareTabIndex(left_path, right_path);
        existing_index.has_value()) {
      operations_.sync_active_editor_tab();
      ActivateCompareTab(*existing_index, false);
      return true;
    }
  }

  auto compare_tab = operations_.build_plain_compare_tab(std::move(left), std::move(right));
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*compare_tab));
  SelectFirstChangeOnOpen(state_.focused_group().open_tabs.back());
  ActivateCompareTab(state_.focused_group().open_tabs.size() - 1, false);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

void DiffTabCoordinator::ActivateCompareTab(std::size_t index, bool dismiss_overlay) {
  state_.focused_group().active_tab_index = index;
  operations_.reveal_active_compare_selection();
  operations_.ensure_active_tab_visible();
  if (dismiss_overlay) {
    operations_.dismiss_overlay(true);
  } else {
    state_.surface.focus = FocusTarget::Editor;
  }
  operations_.request_active_tab_redraw(false);
}

void DiffTabCoordinator::ActivateMergeTab(std::size_t index) {
  state_.focused_group().active_tab_index = index;
  operations_.reveal_active_merge_selection();
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_active_tab_redraw(false);
}

void DiffTabCoordinator::RefreshExistingCompareTab(std::size_t index,
                                                   const std::filesystem::path& normalized_path,
                                                   bool only_when_clean) {
  if (!state_.focused_group().open_tabs[index].compare.has_value()) {
    return;
  }
  if (only_when_clean && state_.focused_group().open_tabs[index].compare->right_viewport.dirty()) {
    return;
  }
  auto rebuilt = operations_.rebuild_compare_tab_entry(normalized_path,
                                                       state_.focused_group().open_tabs[index].compare.value());
  if (rebuilt.has_value() && rebuilt->compare.has_value()) {
    state_.focused_group().open_tabs[index] = std::move(*rebuilt);
  }
}

void DiffTabCoordinator::RestoreMergeViewState(MergeTabState& rebuilt_merge,
                                               const MergeTabState& previous_merge) {
  rebuilt_merge.selected_hunk =
      rebuilt_merge.conflicts.empty()
          ? 0
          : std::min(previous_merge.selected_hunk, rebuilt_merge.conflicts.size() - 1);
  rebuilt_merge.scroll_row = previous_merge.scroll_row;
  rebuilt_merge.horizontal_scroll = previous_merge.horizontal_scroll;
  rebuilt_merge.left_divider_fraction = previous_merge.left_divider_fraction;
  rebuilt_merge.right_divider_fraction = previous_merge.right_divider_fraction;
  rebuilt_merge.persistable = previous_merge.persistable;
  rebuilt_merge.result_viewport.SetViewportSize(previous_merge.result_viewport.visible_lines(),
                                                previous_merge.result_viewport.visible_columns());
  rebuilt_merge.result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, rebuilt_merge.scroll_row)));
  rebuilt_merge.result_viewport.SetHorizontalScroll(rebuilt_merge.horizontal_scroll);
  rebuilt_merge.scroll_row = static_cast<int>(rebuilt_merge.result_viewport.scroll_line());
  rebuilt_merge.horizontal_scroll = rebuilt_merge.result_viewport.horizontal_scroll();
}

void DiffTabCoordinator::OpenComparison(const project::GitCommitEntry& commit) {
  const std::vector<std::filesystem::path> review_files =
      project::CollectGitCommitChangedFiles(state_.root, commit.hash);
  if (const auto existing_index =
          FindOpenCompareTabIndex(state_.overlay.workflow.compare_picker.path, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, state_.overlay.workflow.compare_picker.path, false);
    ActivateCompareTab(*existing_index, true);
    return;
  }

  auto compare_tab = operations_.build_compare_tab_entry(state_.overlay.workflow.compare_picker.path,
                                                         commit, 0);
  if (!compare_tab.has_value()) {
    return;
  }

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*compare_tab));
  TabEntry& opened = state_.focused_group().open_tabs.back();
  if (opened.compare.has_value()) {
    opened.compare->opened_from_commit_picker = true;
    opened.compare->review_mode = compare::CompareReviewMode::Commit;
    opened.compare->review_files = review_files;
    const auto current = std::find(review_files.begin(), review_files.end(),
                                   util::NormalizedPath(opened.compare->path));
    if (current != review_files.end()) {
      opened.compare->review_file_index =
          static_cast<std::size_t>(current - review_files.begin());
    }
  }
  // After the review metadata, not before: it is an input to the presentation the
  // first-change row is expressed in.
  SelectFirstChangeOnOpen(opened);
  ActivateCompareTab(state_.focused_group().open_tabs.size() - 1, true);
  NotifyBufferOpenForEditableTab(opened, operations_);
}

bool DiffTabCoordinator::OpenMergeEditor(const std::filesystem::path& base_path,
                                         const std::filesystem::path& incoming_path,
                                         const std::filesystem::path& current_path,
                                         const std::filesystem::path& output_path) {
  const std::filesystem::path normalized_base = util::NormalizedPath(base_path);
  const std::filesystem::path normalized_incoming = util::NormalizedPath(incoming_path);
  const std::filesystem::path normalized_current = util::NormalizedPath(current_path);
  const std::filesystem::path normalized_output = util::NormalizedPath(output_path);

  if (const auto existing_index = FindOpenMergeTabIndex(normalized_output); existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    if (state_.focused_group().open_tabs[*existing_index].merge.has_value() &&
        !state_.focused_group().open_tabs[*existing_index].merge->result_viewport.dirty()) {
      auto rebuilt = operations_.build_merge_tab_entry(normalized_base, normalized_incoming,
                                                       normalized_current, normalized_output);
      if (rebuilt.has_value() && rebuilt->merge.has_value()) {
        RestoreMergeViewState(rebuilt->merge.value(),
                              state_.focused_group().open_tabs[*existing_index].merge.value());
        state_.focused_group().open_tabs[*existing_index] = std::move(*rebuilt);
      }
    }
    ActivateMergeTab(*existing_index);
    return true;
  }

  auto merge_tab = operations_.build_merge_tab_entry(normalized_base, normalized_incoming,
                                                     normalized_current, normalized_output);
  if (!merge_tab.has_value()) {
    return false;
  }

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.focused_group().open_tabs.size() - 1);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

bool DiffTabCoordinator::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                                   const std::string& left_ref,
                                                   const std::string& left_label) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(state_.root, normalized_path, left_ref);
  if (!left_content.has_value() || left_content->truncated) {
    // Absent revision, or a blob clipped at the subprocess capture ceiling: refuse
    // rather than diff partial bytes as if they were the file's full content.
    return false;
  }
  // Only a genuinely-absent working-tree file becomes an empty (deleted) side; an
  // unreadable or binary file is an error state, not a whole-file-deleted diff, and
  // must not be openable as an editable compare that could save false empty content.
  const util::TextFileReadResult working = util::ReadTextFileClassified(normalized_path);
  if (working.is_error()) {
    return false;
  }
  auto compare_tab = operations_.build_compare_tab_from_buffers(
      normalized_path, left_content->exists ? left_content->content : "", working.content,
      left_label, "Working tree", 0, true, left_content->exists,
      working.status != util::TextFileReadStatus::Missing);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = "WORKTREE";
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;
  compare_tab->compare->right_editable = true;
  compare_tab->compare->right_view_active = true;
  compare_tab->compare->review_mode = compare::CompareReviewMode::WorkingTree;
  compare_tab->compare->staging_view =
      compare::InferWorkingTreeStagingView(left_ref, compare_tab->compare->right_ref);

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*compare_tab));
  SelectFirstChangeOnOpen(state_.focused_group().open_tabs.back());
  ActivateCompareTab(state_.focused_group().open_tabs.size() - 1, false);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

bool DiffTabCoordinator::OpenBranchHeadComparison(const std::filesystem::path& path,
                                                  const std::string& left_ref,
                                                  const std::string& left_label,
                                                  const std::string& right_ref,
                                                  const std::string& right_label) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, right_ref);
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(state_.root, normalized_path, left_ref);
  const auto right_content = project::ReadGitFileAtCommit(state_.root, normalized_path, right_ref);
  if (!left_content.has_value() || !right_content.has_value() || left_content->truncated ||
      right_content->truncated) {
    // A truncated blob was clipped at the subprocess capture ceiling; refuse rather
    // than present a partial diff as truth.
    return false;
  }
  auto compare_tab = operations_.build_compare_tab_from_buffers(
      normalized_path, left_content->exists ? left_content->content : "",
      right_content->exists ? right_content->content : "", left_label, right_label, 0, true,
      left_content->exists, right_content->exists);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = right_ref;
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;
  compare_tab->compare->review_mode = compare::CompareReviewMode::Branch;
  compare_tab->compare->review_files =
      [&]() {
        std::vector<std::filesystem::path> paths;
        for (const project::GitBranchFileEntry& entry :
             project::CollectGitBranchOutgoingFiles(state_.root, left_ref)) {
          paths.push_back(entry.relative_path);
        }
        return paths;
      }();
  const auto current = std::find(compare_tab->compare->review_files.begin(),
                                 compare_tab->compare->review_files.end(), normalized_path);
  if (current != compare_tab->compare->review_files.end()) {
    compare_tab->compare->review_file_index =
        static_cast<std::size_t>(current - compare_tab->compare->review_files.begin());
  }

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*compare_tab));
  SelectFirstChangeOnOpen(state_.focused_group().open_tabs.back());
  ActivateCompareTab(state_.focused_group().open_tabs.size() - 1, false);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

bool DiffTabCoordinator::OpenGitConflictMerge(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path);
      existing_index.has_value() && state_.focused_group().open_tabs[*existing_index].merge.has_value() &&
      state_.focused_group().open_tabs[*existing_index].merge->result_viewport.dirty()) {
    operations_.sync_active_editor_tab();
    ActivateMergeTab(*existing_index);
    return true;
  }

  const auto base_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":1");
  const auto current_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":2");
  const auto incoming_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":3");
  if (!current_content.has_value() || !incoming_content.has_value() ||
      current_content->truncated || incoming_content->truncated ||
      (base_content.has_value() && base_content->truncated)) {
    // A truncated conflict-stage blob was clipped at the subprocess capture ceiling;
    // refuse rather than build a merge from partial bytes.
    return false;
  }

  auto merge_tab = operations_.build_merge_tab_from_buffers(
      normalized_path, base_content.has_value() && base_content->exists ? base_content->content : "",
      incoming_content->exists ? incoming_content->content : "",
      current_content->exists ? current_content->content : "", "Theirs", "Result", "Ours", 0,
      false);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    return false;
  }
  merge_tab->merge->base_path = normalized_path;
  merge_tab->merge->incoming_path = normalized_path;
  merge_tab->merge->current_path = normalized_path;
  if (operations_.finalize_git_merge_tab) {
    operations_.finalize_git_merge_tab(*merge_tab->merge, normalized_path);
  }

  operations_.sync_active_editor_tab();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path); existing_index.has_value()) {
    if (state_.focused_group().open_tabs[*existing_index].merge.has_value()) {
      RestoreMergeViewState(merge_tab->merge.value(),
                            state_.focused_group().open_tabs[*existing_index].merge.value());
    }
    state_.focused_group().open_tabs[*existing_index] = std::move(*merge_tab);
    ActivateMergeTab(*existing_index);
    return true;
  }

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;  // Per-group tab ceiling; see kMaxOpenTabsPerGroup.
  }
  state_.focused_group().open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.focused_group().open_tabs.size() - 1);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

}  // namespace microide::workspace
