#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitRepositoryState.h"

namespace microide::project {

enum class PatchOperationKind {
  StageFile,
  StageHunk,
  StageSelectedLines,
  UnstageFile,
  UnstageHunk,
  UnstageSelectedLines,
  DiscardHunk,
  DiscardSelectedLines,
};

enum class PatchApplySurface {
  Index,
  Worktree,
};

enum class PatchApplyResultCategory {
  Success,
  Cancelled,
  StaleDiff,
  StaleGeneration,
  PatchDidNotApply,
  UnsupportedTarget,
  UnknownError,
};

struct PatchLineSelection {
  std::size_t first_model_row = 0;
  std::size_t last_model_row = 0;
};

struct PatchHunkTarget {
  int hunk_index = -1;
};

struct PatchApplyTarget {
  std::filesystem::path repository_root;
  std::filesystem::path relative_path;
  compare::CompareReviewMode review_mode = compare::CompareReviewMode::WorkingTree;
  compare::WorkingTreeStagingView staging_view = compare::WorkingTreeStagingView::Combined;
  compare::CompareSemanticFileMetadata semantic_file{};
  bool hunk_scope = false;
  std::optional<PatchHunkTarget> hunk;
  std::optional<PatchLineSelection> line_selection;
};

struct PatchApplyRequest {
  PatchOperationKind operation = PatchOperationKind::StageHunk;
  PatchApplyTarget target{};
  compare::CompareModel model{};
  std::uint64_t repository_snapshot_generation = 0;
  std::uint64_t diff_model_generation = 0;
};

struct PatchApplyPreview {
  std::string patch_text;
  std::string summary;
};

struct PatchApplyResult {
  PatchApplyResultCategory category = PatchApplyResultCategory::UnknownError;
  std::string detail;
  std::uint64_t completed_repository_generation = 0;
};

bool PatchOperationAppliesToIndex(PatchOperationKind operation);
bool PatchOperationAppliesToWorktree(PatchOperationKind operation);
bool PatchOperationReversesPatch(PatchOperationKind operation);
PatchApplySurface PatchOperationApplySurface(PatchOperationKind operation);
GitOperationResultCategory MapPatchResultToGitCategory(PatchApplyResultCategory category);
const char* PatchApplyResultCategoryLabel(PatchApplyResultCategory category);

}  // namespace microide::project
