#include "workspace/git/MergeResolverContext.h"

#include "project/GitCommandUtil.h"
#include "util/StringUtil.h"
#include "project/GitRepositoryState.h"
#include "workspace/WorkspacePathUtils.h"

#include <optional>

namespace microide::workspace {
namespace {

std::string BranchLabel(const project::GitRepositoryState& state) {
  if (!state.repo_available) {
    return {};
  }
  switch (state.branch.head_kind) {
    case project::GitHeadKind::Detached:
      if (!state.branch.branch_name.empty()) {
        return state.branch.branch_name;
      }
      if (state.branch.head_oid.size() >= 7) {
        return "detached @ " + state.branch.head_oid.substr(0, 7);
      }
      return "detached";
    case project::GitHeadKind::Unborn:
      return "(unborn)";
    case project::GitHeadKind::Normal:
      if (!state.branch.branch_name.empty()) {
        return state.branch.branch_name;
      }
      break;
  }
  if (state.branch.head_oid.size() >= 7) {
    return state.branch.head_oid.substr(0, 7);
  }
  return "HEAD";
}

std::string IncomingRefLabel(const std::filesystem::path& project_root,
                             const project::GitRepositoryState& repository_state) {
  // Read `<gitdir>/MERGE_HEAD` directly instead of forking `git rev-parse
  // --short MERGE_HEAD`. This runs on the shell thread (FinalizeGitMergeTab), so
  // the subprocess it replaced could stall the UI for up to kGitReadTimeoutMs
  // (60 s) on a wedged git — for a pane caption. Abbreviating to 7 characters
  // matches BranchLabel's own detached-HEAD abbreviation above.
  if (const std::optional<std::string> merge_head =
          project::internal::ReadPendingMergeHeadId(project_root);
      merge_head.has_value()) {
    return merge_head->substr(0, 7);
  }
  if (repository_state.operation_state == project::GitOperationStateKind::Rebase ||
      repository_state.operation_state == project::GitOperationStateKind::CherryPick) {
    return "upstream";
  }
  return "incoming";
}

std::string PaneLabel(std::string_view role, std::string_view ref_context) {
  if (ref_context.empty()) {
    return std::string(role);
  }
  return std::string(role) + " (" + std::string(ref_context) + ")";
}

}  // namespace

MergeResolverLabels BuildMergeResolverLabels(const std::filesystem::path& project_root,
                                             const std::filesystem::path& output_path,
                                             const project::GitRepositoryState& repository_state) {
  const std::string current_ref = BranchLabel(repository_state);
  const std::string incoming_ref = IncomingRefLabel(project_root, repository_state);
  const std::string path_label = RelativePathLabel(project_root, output_path);
  return MergeResolverLabels{
      .incoming_label = PaneLabel("Incoming", incoming_ref),
      .current_label = PaneLabel("Current", current_ref),
      .result_label = PaneLabel("Result", path_label),
      .base_label = "Base (common ancestor)",
  };
}

MergeResolverStatus BuildMergeResolverStatus(std::string& label_buffer,
                                             const MergeTabState& merge_tab,
                                             std::size_t remaining_conflicted_files) {
  MergeResolverStatus status;
  status.selected_conflict_index =
      merge_tab.conflicts.empty()
          ? 0
          : std::min(merge_tab.selected_hunk, merge_tab.conflicts.size() - 1) + 1;
  status.total_conflicts = merge_tab.conflicts.size();
  status.remaining_conflicts = CountRemainingMergeConflicts(merge_tab.conflicts);
  status.remaining_files = remaining_conflicted_files;
  status.result_state = ComputeMergeResultState(merge_tab, merge_tab.file_conflict);

  switch (status.result_state) {
    case MergeResultState::Dirty:
      status.result_state_label = "dirty";
      break;
    case MergeResultState::Saved:
      status.result_state_label = "saved";
      break;
    case MergeResultState::Invalid:
      status.result_state_label = "invalid";
      break;
    case MergeResultState::Resolved:
      status.result_state_label = "resolved";
      break;
    case MergeResultState::Stale:
      status.result_state_label = "stale";
      break;
  }

  // Appended into the caller's buffer rather than composed with `+`. The old
  // spelling built four temporaries per call for one toolbar line, and the
  // merge surface calls this on every painted frame.
  std::string& label = label_buffer;
  label.clear();
  if (status.total_conflicts == 0) {
    // A literal, so the buffer is not even touched in the no-conflict case.
    status.progress_label = "No merge conflicts";
    return status;
  }
  label += "Conflict ";
  util::AppendUnsigned(label, status.selected_conflict_index);
  label += '/';
  util::AppendUnsigned(label, status.total_conflicts);
  label += " | remaining ";
  util::AppendUnsigned(label, status.remaining_conflicts);
  if (remaining_conflicted_files > 0) {
    label += " | files ";
    util::AppendUnsigned(label, remaining_conflicted_files);
  }
  label += " | ";
  label += status.result_state_label;
  status.progress_label = label;
  return status;
}

std::optional<project::GitRepositoryEntry> FindConflictRepositoryEntry(
    const project::GitRepositoryState& repository_state,
    const std::filesystem::path& relative_path) {
  // Repository entries carry generic text, not a `path` (TD-2026-08-11-183), so
  // derive the key once here instead of per entry.
  const std::string key = relative_path.generic_string();
  for (const project::GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.conflicted && entry.path.relative_path == key) {
      return entry;
    }
    if (entry.conflicted && entry.old_path.has_value() &&
        entry.old_path->relative_path == key) {
      return entry;
    }
  }
  return std::nullopt;
}

std::span<const std::string> EnsureMergePreviewLines(MergeTabState& merge_tab,
                                                     std::size_t conflict_index,
                                                     compare::MergeChoice choice) {
  if (conflict_index >= merge_tab.conflicts.size()) {
    return {};
  }
  const MergeTrackedConflict& conflict = merge_tab.conflicts[conflict_index];
  if (!conflict.valid || conflict.hunk_index >= merge_tab.model.hunks.size()) {
    return {};
  }
  if (merge_tab.preview_lines_cache_valid &&
      merge_tab.preview_lines_cache_conflict == conflict_index &&
      merge_tab.preview_lines_cache_choice == choice &&
      merge_tab.preview_lines_cache_revision == merge_tab.model_revision) {
    return merge_tab.preview_lines_cache;
  }
  merge_tab.preview_lines_cache =
      compare::MergeChoiceLines(merge_tab.model.hunks[conflict.hunk_index], choice);
  merge_tab.preview_lines_cache_valid = true;
  merge_tab.preview_lines_cache_conflict = conflict_index;
  merge_tab.preview_lines_cache_choice = choice;
  merge_tab.preview_lines_cache_revision = merge_tab.model_revision;
  return merge_tab.preview_lines_cache;
}

}  // namespace microide::workspace
