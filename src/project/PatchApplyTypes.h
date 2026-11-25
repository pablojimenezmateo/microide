#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "platform/ProcessLauncher.h"

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitRepositoryState.h"

namespace microide::project {

enum class PatchOperationKind {
  StageHunk,
  StageSelectedLines,
  UnstageHunk,
  UnstageSelectedLines,
  DiscardHunk,
  DiscardSelectedLines,
};

enum class PatchApplyResultCategory {
  Success,
  StaleDiff,
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

struct PatchChangeSpan {
  std::size_t head_first = 1;
  std::size_t head_last = 0;
  std::size_t worktree_first = 1;
  std::size_t worktree_last = 0;
  bool covers_end = false;
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
  std::optional<PatchChangeSpan> change_span;
  compare::CompareTextBuffer working_tree_source;
  bool working_tree_exists = true;
};

struct PatchApplyRequest {
  PatchOperationKind operation = PatchOperationKind::StageHunk;
  PatchApplyTarget target{};
  compare::CompareModel model{};
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
};

struct PatchApplyPreview {
  std::string patch_text;
  std::string summary;
};

struct PatchApplyResult {
  PatchApplyResultCategory category = PatchApplyResultCategory::UnknownError;
  std::string detail;
};

bool PatchOperationAppliesToIndex(PatchOperationKind operation);
bool PatchOperationReversesPatch(PatchOperationKind operation);
}
