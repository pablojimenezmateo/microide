#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "compare/CompareReviewTypes.h"
#include "project/GitCompareService.h"
#include "util/PathMatch.h"
#include "util/TextFileIO.h"
#include "workspace/git/CompareTabReview.h"
#include "util/InvokeIfSet.h"

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

void SelectFirstChangeOnOpen(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
    return;
  }
  CompareTabState& compare_tab = *tab.compare;
  compare_tab.selected_row = CompareFirstChangePresentationRow(compare_tab);
  NormalizeCompareSelectionToModelRow(compare_tab);
  SyncCompareCaretToSelectedRow(compare_tab);
}

std::size_t IndexOfReviewFile(const std::vector<std::filesystem::path>& review_files,
                              const std::filesystem::path& absolute_path,
                              const std::filesystem::path& root) {
  const std::optional<std::string> relative = util::RelativePathWithin(absolute_path, root);
  if (!relative.has_value()) {
    return 0;
  }
  const std::filesystem::path relative_path =
      std::filesystem::path(*relative).lexically_normal();
  const auto found = std::find(review_files.begin(), review_files.end(), relative_path);
  return found == review_files.end()
             ? 0
             : static_cast<std::size_t>(found - review_files.begin());
}

std::optional<compare::CompareReviewMode> OwedReviewFiles(const CompareTabState& compare) {
  const bool collected = compare.review_mode == compare::CompareReviewMode::Commit ||
                         compare.review_mode == compare::CompareReviewMode::Branch;
  return collected && compare.review_files.empty() ? std::optional(compare.review_mode) : std::nullopt;
}

bool ComparesTwoFixedRevisions(const CompareTabState& compare) {
  return project::IsFullGitObjectId(compare.commit_hash) && project::IsFullGitObjectId(compare.right_ref);
}

}

std::optional<CompareInput> ReadFileCompareInput(const std::filesystem::path& path, bool editable) {
  const std::filesystem::path normalized = util::NormalizedPath(path);
  const util::TextFileReadResult read = util::ReadTextFileClassified(normalized);
  if (read.is_error()) {
    return std::nullopt;
  }
  return CompareInput{
      .content = read.ok() ? read.content : std::string{},
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
  return AddCompareTab(std::move(*compare_tab), false);
}

bool DiffTabCoordinator::LoadsOffThread(const std::filesystem::path& normalized_path,
                                        const project::GitRevisionBlobCache* prefetched) const {
  if (!operations_.begin_compare_tab_load) {
    return false;
  }
  if (prefetched == nullptr) {
    return true;
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(normalized_path, error);
  return !error && size >= kAsyncCompareThresholdBytes;
}

bool DiffTabCoordinator::AddCompareTab(TabEntry tab, bool load_off_thread) {
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(tab));
  if (!load_off_thread) {
    SelectFirstChangeOnOpen(state_.focused_group().open_tabs.back());
  }
  ActivateCompareTab(state_.focused_group().open_tabs.size() - 1, false);
  if (!load_off_thread) {
    NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  }
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
  operations_.request_frame();
}

void DiffTabCoordinator::ActivateMergeTab(std::size_t index) {
  state_.focused_group().active_tab_index = index;
  operations_.reveal_active_merge_selection();
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_frame();
}

void DiffTabCoordinator::RefreshExistingCompareTab(std::size_t index,
                                                   const std::filesystem::path& normalized_path,
                                                   bool only_when_clean) {
  if (!state_.focused_group().open_tabs[index].compare.has_value()) {
    return;
  }
  CompareTabState& compare = *state_.focused_group().open_tabs[index].compare;
  if ((only_when_clean && compare.right_viewport.dirty()) || compare.load_pending ||
      ComparesTwoFixedRevisions(compare)) {
    return;
  }
  if (CompareRefreshReadsGit(compare) && operations_.begin_compare_tab_refresh) {
    compare.refresh_owed = true;
    (void)StartCompareTabRefresh(compare);
    return;
  }
  auto rebuilt = operations_.rebuild_compare_tab_entry(normalized_path, compare, nullptr);
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
  const std::filesystem::path path = state_.overlay.workflow.compare_picker.path;
  if (const auto existing_index = FindOpenCompareTabIndex(path, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, util::NormalizedPath(path), false);
    ActivateCompareTab(*existing_index, true);
    return;
  }
  if (!OpenWorkingTreeComparison(path, commit.hash, commit.short_hash)) {
    return;
  }
  TabEntry& opened = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (opened.compare.has_value()) {
    CompareTabState& compare = *opened.compare;
    compare.opened_from_commit_picker = true;
    compare.review_mode = compare::CompareReviewMode::Commit;
    if (!compare.load_pending) {
      compare.review_files = project::CollectGitCommitChangedFiles(state_.root, state_.launcher(), commit.hash);
      compare.review_file_index = IndexOfReviewFile(compare.review_files, compare.path, state_.root);
    }
  }
  operations_.dismiss_overlay(true);
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
    return false;
  }
  operations_.sync_active_editor_tab();
  state_.focused_group().open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.focused_group().open_tabs.size() - 1);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

bool DiffTabCoordinator::OpenWorkingTreeComparison(
    const std::filesystem::path& path,
    const std::string& left_ref,
    const std::string& left_label,
    const project::GitRevisionBlobCache* prefetched) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const bool load_off_thread = LoadsOffThread(normalized_path, prefetched);

  std::optional<TabEntry> compare_tab;
  if (load_off_thread) {
    compare_tab = operations_.build_compare_tab_from_buffers(
        normalized_path, "", "", left_label, "Working tree", 0, true, true, true);
  } else {
    const auto left_content =
        project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, left_ref,
                                     prefetched);
    if (!left_content.has_value() || left_content->truncated) {
      return false;
    }
    const util::TextFileReadResult working = util::ReadTextFileClassified(normalized_path);
    if (working.is_error()) {
      return false;
    }
    compare_tab = operations_.build_compare_tab_from_buffers(
        normalized_path, left_content->exists ? left_content->content : "", working.content,
        left_label, "Working tree", 0, true, left_content->exists,
        working.status != util::TextFileReadStatus::Missing);
  }
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
  if (load_off_thread) {
    compare_tab->compare->right_viewport.SetReadOnly(true);
    compare_tab->compare->load_pending = true;
  } else {
    (void)AdoptLiveBufferForCompareRightSide(state_.editor_groups, *compare_tab->compare);
    util::InvokeIfSet(operations_.refresh_compare_tab_derived_state, *compare_tab->compare);
  }

  return AddCompareTab(std::move(*compare_tab), load_off_thread);
}

void DiffTabCoordinator::RefreshCompareTabsForPath(const std::filesystem::path& normalized_path) {
  for (std::size_t group_index = 0; group_index < state_.editor_groups.size(); ++group_index) {
    EditorGroup& group = state_.editor_groups[group_index];
    for (std::size_t index = 0; index < group.open_tabs.size(); ++index) {
      TabEntry& tab = group.open_tabs[index];
      if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() || tab.compare->plain_compare ||
          tab.compare->path != normalized_path) {
        continue;
      }
      CompareTabState& compare = *tab.compare;
      if (compare.load_pending) {
        if (compare.pending_load.armed()) {
          operations_.cancel_file_read(compare.pending_load.id());
          compare.pending_load.Disarm();
        }
        continue;
      }
      if (ComparesTwoFixedRevisions(compare)) {
        continue;
      }
      if (CompareRefreshReadsGit(compare) && operations_.begin_compare_tab_refresh) {
        if (compare.pending_refresh.armed()) {
          operations_.cancel_file_read(compare.pending_refresh.id());
          compare.pending_refresh.Disarm();
        }
        compare.refresh_owed = true;
        (void)StartCompareTabRefresh(compare);
        continue;
      }
      auto rebuilt = operations_.rebuild_compare_tab_entry(normalized_path, compare, nullptr);
      if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
        continue;
      }
      group.open_tabs[index] = std::move(*rebuilt);
      if (index == group.active_tab_index) {
        if (group_index == state_.focused_group_index) {
          operations_.reveal_active_compare_selection();
        }
        operations_.request_frame();
      }
    }
  }
}

bool DiffTabCoordinator::StartCompareTabRefresh(CompareTabState& compare_tab) {
  if (!compare_tab.refresh_owed || compare_tab.pending_refresh.armed() || compare_tab.load_pending ||
      !operations_.begin_compare_tab_refresh) {
    return false;
  }
  const auto side_path = [&](const std::filesystem::path& path) {
    return (path.empty() ? compare_tab.path : path).lexically_normal();
  };
  const std::uint64_t id = operations_.begin_compare_tab_refresh(CompareTabRefreshRequest{
      .path = compare_tab.path,
      .root = state_.root,
      .launcher = &state_.launcher(),
      .left_path = side_path(compare_tab.left_path),
      .left_ref = compare_tab.commit_hash,
      .left_held = CompareLeftSideIsHeld(compare_tab),
      .right_path = side_path(compare_tab.right_path),
      .right_ref = compare_tab.right_ref,
  });
  if (id == 0) {
    return false;
  }
  compare_tab.pending_refresh.Arm(id, 0);
  return true;
}

bool DiffTabCoordinator::ApplyCompareTabRefresh(const std::uint64_t id, const bool cancelled,
                                                const CompareRevisionSides& sides) {
  for (std::size_t group_index = 0; group_index < state_.editor_groups.size(); ++group_index) {
    EditorGroup& group = state_.editor_groups[group_index];
    for (std::size_t index = 0; index < group.open_tabs.size(); ++index) {
      TabEntry& tab = group.open_tabs[index];
      if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
          !tab.compare->pending_refresh.Holds(id)) {
        continue;
      }
      CompareTabState& compare = *tab.compare;
      if (compare.pending_refresh.Resolve(id, 0) != editor::AsyncBufferWork::Claim::Current || cancelled) {
        return true;
      }
      compare.refresh_owed = false;
      auto rebuilt = operations_.rebuild_compare_tab_entry(compare.path, compare, &sides);
      if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
        return true;
      }
      group.open_tabs[index] = std::move(*rebuilt);
      if (index == group.active_tab_index) {
        if (group_index == state_.focused_group_index) {
          operations_.reveal_active_compare_selection();
        }
        operations_.request_frame();
      }
      return true;
    }
  }
  return false;
}

bool DiffTabCoordinator::StartPendingCompareLoad(CompareTabState& compare_tab) {
  if (!compare_tab.load_pending || compare_tab.pending_load.armed() ||
      !operations_.begin_compare_tab_load) {
    return false;
  }
  const std::uint64_t id = operations_.begin_compare_tab_load(CompareTabLoadRequest{
      .path = compare_tab.path,
      .root = state_.root,
      .launcher = &state_.launcher(),
      .left_path = compare_tab.left_path,
      .left_ref = compare_tab.commit_hash,
      .right_path = compare_tab.right_path,
      .right_ref = compare_tab.right_ref,
      .right_held = compare_tab.right_viewport.dirty(),
      .build_options = compare_tab.build_options,
      .collect_review_files = OwedReviewFiles(compare_tab),
  });
  if (id == 0) {
    return false;
  }
  compare_tab.pending_load.Arm(id, compare_tab.right_viewport.content_revision());
  return true;
}

void DiffTabCoordinator::ApplyCompareTabLoad(const std::uint64_t id, const bool cancelled,
                                             const bool read_ok, CompareTabLoadResult& result) {
  for (std::size_t group_index = 0; group_index < state_.editor_groups.size(); ++group_index) {
    EditorGroup& group = state_.editor_groups[group_index];
    for (std::size_t tab_index = 0; tab_index < group.open_tabs.size(); ++tab_index) {
      TabEntry& tab = group.open_tabs[tab_index];
      if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
          !tab.compare->pending_load.Holds(id)) {
        continue;
      }
      CompareTabState& compare = *tab.compare;
      if (compare.pending_load.Resolve(id, compare.right_viewport.content_revision()) !=
          editor::AsyncBufferWork::Claim::Current) {
        return;
      }
      if (cancelled) {
        return;
      }
      compare.load_pending = false;
      const bool right_held = compare.right_viewport.dirty();
      if (right_held) {
        compare.right_viewport.SetReadOnly(false);
      }
      if (!read_ok || !result.left_ok || !result.right_ok) {
        if (operations_.report_compare_load_failure) {
          const std::string name = compare.path.filename().string();
          std::string message;
          if (!read_ok) {
            message = "Could not read " + name;
          } else if (!result.left_ok) {
            message = "Could not read " + name + " at " + compare.commit_hash;
          } else if (!result.right_read) {
            message = compare.right_ref == "WORKTREE" ? "Could not read " + name
                                                      : "Could not read " + name + " at " + compare.right_ref;
          } else {
            message = "Cannot compare " + name + ": not a text file";
          }
          operations_.report_compare_load_failure(std::move(message));
        }
        operations_.request_frame();
        return;
      }
      compare.left_content = std::move(result.left);
      compare.build_options.left_exists = result.built_options.left_exists;
      compare.build_options.syntax = result.built_options.syntax;
      if (right_held) {
        compare.derived_fingerprint_valid = false;
      } else {
        compare.build_options.right_exists = result.built_options.right_exists;
        compare.right_viewport = std::move(result.right);
        util::InvokeIfSet(operations_.apply_editor_preferences, compare.right_viewport);
        compare.model = std::move(result.model);
        MarkCompareModelBuilt(compare, result.built_options, result.left_line_count);
      }
      if (result.review_files.has_value()) {
        compare.review_files = std::move(*result.review_files);
        compare.review_file_index = IndexOfReviewFile(compare.review_files, compare.path, state_.root);
      }
      AdoptLiveBufferForCompareRightSide(state_.editor_groups, compare,
                                         &compare.right_viewport);
      util::InvokeIfSet(operations_.refresh_compare_tab_derived_state, compare);
      if (compare.keep_view_on_load) {
        compare.keep_view_on_load = false;
        NormalizeCompareSelectionToModelRow(compare);
        SyncCompareCaretToSelectedRow(compare);
      } else {
        SelectFirstChangeOnOpen(tab);
      }
      NotifyBufferOpenForEditableTab(tab, operations_);
      operations_.request_frame();
      return;
    }
  }
}

bool DiffTabCoordinator::OpenBranchHeadComparison(
    const std::filesystem::path& path,
    const std::string& left_ref,
    const std::string& left_label,
    const std::string& right_ref,
    const std::string& right_label,
    const project::GitRevisionBlobCache* prefetched,
    const std::vector<std::filesystem::path>* review_files) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, right_ref);
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const bool load_off_thread =
      LoadsOffThread(normalized_path, prefetched) || (review_files == nullptr && operations_.begin_compare_tab_load);

  std::optional<TabEntry> compare_tab;
  if (load_off_thread) {
    compare_tab = operations_.build_compare_tab_from_buffers(
        normalized_path, "", "", left_label, right_label, 0, true, true, true);
  } else {
    const auto left_content =
        project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, left_ref,
                                     prefetched);
    const auto right_content =
        project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, right_ref,
                                     prefetched);
    if (!left_content.has_value() || !right_content.has_value() || left_content->truncated ||
        right_content->truncated) {
      return false;
    }
    compare_tab = operations_.build_compare_tab_from_buffers(
        normalized_path, left_content->exists ? left_content->content : "",
        right_content->exists ? right_content->content : "", left_label, right_label, 0, true,
        left_content->exists, right_content->exists);
  }
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  if (load_off_thread) {
    compare_tab->compare->right_viewport.SetReadOnly(true);
    compare_tab->compare->load_pending = true;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = right_ref;
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;
  compare_tab->compare->review_mode = compare::CompareReviewMode::Branch;
  if (review_files != nullptr) {
    compare_tab->compare->review_files = *review_files;
  } else if (!load_off_thread) {
    for (const project::GitBranchFileEntry& entry :
         project::CollectGitBranchOutgoingFiles(state_.root, state_.launcher(), left_ref)) {
      compare_tab->compare->review_files.push_back(entry.relative_path);
    }
  }
  compare_tab->compare->review_file_index =
      IndexOfReviewFile(compare_tab->compare->review_files, normalized_path, state_.root);
  if (!load_off_thread && operations_.refresh_compare_tab_derived_state) {
    operations_.refresh_compare_tab_derived_state(*compare_tab->compare);
  }

  return AddCompareTab(std::move(*compare_tab), load_off_thread);
}

bool DiffTabCoordinator::OpenGitConflictMerge(const std::filesystem::path& path,
                                              const project::GitRevisionBlobCache* prefetched) {
  const std::filesystem::path normalized_path = util::NormalizedPath(path);
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path);
      existing_index.has_value() && state_.focused_group().open_tabs[*existing_index].merge.has_value() &&
      state_.focused_group().open_tabs[*existing_index].merge->result_viewport.dirty()) {
    operations_.sync_active_editor_tab();
    ActivateMergeTab(*existing_index);
    return true;
  }

  if (prefetched == nullptr && operations_.begin_git_conflict_prefetch) {
    operations_.begin_git_conflict_prefetch(normalized_path);
    return true;
  }

  const auto base_content =
      project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, ":1", prefetched);
  const auto current_content =
      project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, ":2", prefetched);
  const auto incoming_content =
      project::ReadGitFileAtCommit(state_.root, state_.launcher(), normalized_path, ":3", prefetched);
  if (!current_content.has_value() || !incoming_content.has_value() ||
      current_content->truncated || incoming_content->truncated ||
      (base_content.has_value() && base_content->truncated)) {
    return false;
  }

  auto merge_tab = operations_.build_merge_tab_from_buffers(
      normalized_path, base_content.has_value() && base_content->exists ? base_content->content : "",
      incoming_content->exists ? incoming_content->content : "",
      current_content->exists ? current_content->content : "", "Remote", "Result", "Local", 0,
      false);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    return false;
  }
  merge_tab->merge->base_path = normalized_path;
  merge_tab->merge->incoming_path = normalized_path;
  merge_tab->merge->current_path = normalized_path;
  util::InvokeIfSet(operations_.finalize_git_merge_tab, *merge_tab->merge, normalized_path);

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
    return false;
  }
  state_.focused_group().open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.focused_group().open_tabs.size() - 1);
  NotifyBufferOpenForEditableTab(state_.focused_group().open_tabs.back(), operations_);
  return true;
}

}
