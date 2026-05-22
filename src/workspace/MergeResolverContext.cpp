#include "workspace/MergeResolverContext.h"

#include "project/GitRepository.h"
#include "workspace/WorkspacePathUtils.h"

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
  project::GitRepository repository(project_root);
  if (!repository.IsValid()) {
    return "incoming";
  }
  const auto merge_head = repository.Execute({"rev-parse", "--short", "MERGE_HEAD"});
  if (merge_head.success() && !merge_head.output.empty()) {
    std::string label = merge_head.output;
    while (!label.empty() && (label.back() == '\n' || label.back() == '\r')) {
      label.pop_back();
    }
    if (!label.empty()) {
      return label;
    }
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

MergeResolverStatus BuildMergeResolverStatus(const MergeTabState& merge_tab,
                                             std::size_t remaining_conflicted_files) {
  MergeResolverStatus status{
      .selected_conflict_index =
          merge_tab.conflicts.empty()
              ? 0
              : std::min(merge_tab.selected_hunk, merge_tab.conflicts.size() - 1) + 1,
      .total_conflicts = merge_tab.conflicts.size(),
      .remaining_conflicts = CountRemainingMergeConflicts(merge_tab.conflicts),
      .remaining_files = remaining_conflicted_files,
      .result_state = ComputeMergeResultState(merge_tab, merge_tab.file_conflict),
  };

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

  if (status.total_conflicts == 0) {
    status.progress_label = "No merge conflicts";
  } else {
    status.progress_label = "Conflict " + std::to_string(status.selected_conflict_index) + "/" +
                            std::to_string(status.total_conflicts) + " | remaining " +
                            std::to_string(status.remaining_conflicts);
    if (remaining_conflicted_files > 0) {
      status.progress_label += " | files " + std::to_string(remaining_conflicted_files);
    }
    status.progress_label += " | " + status.result_state_label;
  }
  return status;
}

std::optional<project::GitRepositoryEntry> FindConflictRepositoryEntry(
    const project::GitRepositoryState& repository_state,
    const std::filesystem::path& relative_path) {
  for (const project::GitRepositoryEntry& entry : repository_state.entries) {
    if (entry.conflicted && entry.path.relative_path == relative_path) {
      return entry;
    }
    if (entry.conflicted && entry.old_path.has_value() &&
        entry.old_path->relative_path == relative_path) {
      return entry;
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace
