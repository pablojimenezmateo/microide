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
  StageHunk,
  StageSelectedLines,
  UnstageHunk,
  UnstageSelectedLines,
  DiscardHunk,
  DiscardSelectedLines,
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

// The change a Combined (HEAD vs working tree) compare tab is asked to stage or
// unstage, as the two line ranges it occupies: HEAD's and the working tree's.
// Lines are 1-based; a side with no lines of its own (a pure insertion has none
// on HEAD's side) is `last == first - 1` at the insertion point. `covers_end`
// says the change reaches the end of the file, so it owns the final-newline
// state too.
//
// A stage goes to the INDEX and an unstage comes out of it, and the index stops
// equalling HEAD as soon as anything is staged — so the tab's own patch, whose
// line numbers are HEAD's, only fits for the first hunk (git refuses to shift a
// hunk anchored at either end of the file, and a hunk already staged would apply
// twice). The apply instead maps this span onto the index as it is now (its
// HEAD lines must still be present there unchanged, else the change is already
// staged), splices the working-tree lines in, and stages the diff from the
// index to that result. An unstage is the mirror image.
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
  // Set for a stage/unstage requested from a Combined (HEAD vs working tree)
  // view: the apply regenerates the patch against the index as it is now (see
  // PatchChangeSpan) instead of using the tab's patch.
  std::optional<PatchChangeSpan> change_span;
  // The working-tree text the tab shows — its editable right side, which may
  // hold unsaved edits — for the stage side of that regeneration. Shared, not
  // copied. Null means read the file from disk.
  compare::CompareTextBuffer working_tree_source;
  // Whether the working-tree file exists (false: the change is its deletion).
  bool working_tree_exists = true;
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
bool PatchOperationReversesPatch(PatchOperationKind operation);
}  // namespace microide::project
