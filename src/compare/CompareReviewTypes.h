#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::compare {

enum class CompareReviewMode {
  WorkingTree,
  Commit,
  Branch,
  Conflict,
  // A non-git ("plain") comparison: two arbitrary sides (file/buffer/clipboard)
  // with no repository backing. Sticky — never re-inferred from refs — so the
  // git review metadata, staging, and branch-review machinery stay disabled.
  Plain,
};

enum class WorkingTreeStagingView {
  Combined,
  Unstaged,
  Staged,
};

enum class CompareSemanticFileKind {
  Text,
  Binary,
  Submodule,
};

struct CompareSemanticFileMetadata {
  CompareSemanticFileKind file_kind = CompareSemanticFileKind::Text;
  bool renamed = false;
  std::filesystem::path old_path;
  std::filesystem::path new_path;
  bool mode_changed = false;
  bool old_executable = false;
  bool new_executable = false;
  bool line_ending_only = false;
  bool submodule_pointer_changed = false;
  std::string old_submodule_oid;
  std::string new_submodule_oid;

  // Lets a caller ask "did the classification actually move?" so downstream work
  // keyed on it (the compare presentation model) can be skipped when it did not.
  bool operator==(const CompareSemanticFileMetadata&) const = default;
};

struct BranchReviewTargetIdentity {
  std::filesystem::path repository_root;
  std::string base_commit;
  std::string head_commit;
  std::string merge_base_commit;
  std::uint64_t snapshot_generation = 0;

  bool operator==(const BranchReviewTargetIdentity& other) const;
  bool operator!=(const BranchReviewTargetIdentity& other) const {
    return !(*this == other);
  }
};

struct CompareBuildOptions {
  bool ignore_whitespace = false;
};

std::string CompareReviewModeLabel(CompareReviewMode mode);
std::string WorkingTreeStagingViewLabel(WorkingTreeStagingView view);
CompareReviewMode InferCompareReviewMode(std::string_view left_ref,
                                         std::string_view right_ref,
                                         bool opened_from_commit_picker);
WorkingTreeStagingView InferWorkingTreeStagingView(std::string_view left_ref,
                                                   std::string_view right_ref);
BranchReviewTargetIdentity MakeBranchReviewTargetIdentity(
    std::filesystem::path repository_root,
    std::string_view base_commit,
    std::string_view head_commit,
    std::string_view merge_base_commit,
    std::uint64_t snapshot_generation);

}  // namespace microide::compare
