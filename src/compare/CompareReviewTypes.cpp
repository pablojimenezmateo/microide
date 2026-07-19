#include "compare/CompareReviewTypes.h"

namespace microide::compare {

namespace {

bool IsGitStageRef(std::string_view ref) {
  return !ref.empty() && ref.front() == ':' && ref.size() <= 3;
}

}  // namespace

std::string CompareReviewModeLabel(CompareReviewMode mode) {
  switch (mode) {
    case CompareReviewMode::WorkingTree:
      return "working-tree";
    case CompareReviewMode::Commit:
      return "commit";
    case CompareReviewMode::Branch:
      return "branch";
    case CompareReviewMode::Conflict:
      return "conflict";
    case CompareReviewMode::Plain:
      return "plain";
  }
  return "working-tree";
}

std::string WorkingTreeStagingViewLabel(WorkingTreeStagingView view) {
  switch (view) {
    case WorkingTreeStagingView::Combined:
      return "combined";
    case WorkingTreeStagingView::Unstaged:
      return "unstaged";
    case WorkingTreeStagingView::Staged:
      return "staged";
  }
  return "combined";
}

CompareReviewMode InferCompareReviewMode(std::string_view left_ref,
                                         std::string_view right_ref,
                                         bool opened_from_commit_picker) {
  if (IsGitStageRef(left_ref) && IsGitStageRef(right_ref)) {
    return CompareReviewMode::Conflict;
  }
  if (right_ref == "WORKTREE") {
    return opened_from_commit_picker ? CompareReviewMode::Commit : CompareReviewMode::WorkingTree;
  }
  if (right_ref == "HEAD" || left_ref == "HEAD") {
    return CompareReviewMode::Branch;
  }
  if (opened_from_commit_picker) {
    return CompareReviewMode::Commit;
  }
  return CompareReviewMode::Branch;
}

WorkingTreeStagingView InferWorkingTreeStagingView(std::string_view left_ref,
                                                   std::string_view right_ref) {
  if (right_ref != "WORKTREE") {
    return WorkingTreeStagingView::Combined;
  }
  if (left_ref == ":0" || left_ref == "INDEX") {
    return WorkingTreeStagingView::Staged;
  }
  if (left_ref == "WORKTREE" || left_ref == ":3") {
    return WorkingTreeStagingView::Unstaged;
  }
  return WorkingTreeStagingView::Combined;
}

BranchReviewTargetIdentity MakeBranchReviewTargetIdentity(
    std::filesystem::path repository_root,
    std::string_view base_commit,
    std::string_view head_commit,
    std::string_view merge_base_commit,
    std::uint64_t snapshot_generation) {
  return BranchReviewTargetIdentity{
      .repository_root = std::move(repository_root),
      .base_commit = std::string(base_commit),
      .head_commit = std::string(head_commit),
      .merge_base_commit = std::string(merge_base_commit),
      .snapshot_generation = snapshot_generation,
  };
}

bool BranchReviewTargetIdentity::operator==(const BranchReviewTargetIdentity& other) const {
  return repository_root == other.repository_root && base_commit == other.base_commit &&
         head_commit == other.head_commit && merge_base_commit == other.merge_base_commit &&
         snapshot_generation == other.snapshot_generation;
}

}  // namespace microide::compare
