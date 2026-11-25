#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"

namespace microide::compare {

enum class ComparePresentationRowKind {
  Model,
  Metadata,
  CollapsedContext,
  HunkHeader,
  // A review note, under the hunk it is on (`hunk_index` >= 0) or at the top:
  // the file's own note (`hunk_index` kCompareFileNoteHunk), then each detached
  // one (CompareDetachedNoteHunk: a hunk note whose change is gone).
  // `summary_text` is the note, for a detached one after its "line N: ".
  ReviewNote,
};

// What a ReviewNote row's `hunk_index` (and a note control's hunk) says when the
// note is not under a hunk.
inline constexpr int kCompareFileNoteHunk = -1;
constexpr int CompareDetachedNoteHunk(std::size_t detached_index) {
  return -2 - static_cast<int>(detached_index);
}
constexpr bool IsCompareDetachedNoteHunk(int hunk) { return hunk <= -2; }
constexpr std::size_t CompareDetachedNoteIndex(int hunk) { return static_cast<std::size_t>(-2 - hunk); }

struct ComparePresentationRow {
  ComparePresentationRowKind kind = ComparePresentationRowKind::Model;
  std::size_t model_row_index = 0;
  std::string summary_text;
  // Render-ready summary: `summary_text` with the review-marker prefix
  // ("[label]* " / "* ") already applied. Composed at presentation build and
  // re-composed by ApplyBranchReviewPresentationMarkers, so the render path
  // never assembles summary strings per frame.
  std::string display_summary_text;
  int hunk_index = -1;
  int collapsed_line_count = 0;
  std::size_t collapsed_run_start_model_row = 0;
  std::size_t collapsed_run_length = 0;
  bool context_above = false;
  int previous_hunk_index = -1;
  int next_hunk_index = -1;
  std::string review_marker_label;
  bool has_review_note = false;
};

struct ComparePresentationCollapsedRunState {
  std::size_t run_start_model_row = 0;
  std::size_t run_length = 0;
  std::size_t expanded_above = 0;
  std::size_t expanded_below = 0;
};

struct ComparePresentationCollapseState {
  std::vector<ComparePresentationCollapsedRunState> collapsed_runs;
};

// Locate the saved expand state for the collapsed run identified by
// (run_start_model_row, run_length), or nullptr if none has been recorded.
ComparePresentationCollapsedRunState* FindCollapsedRunState(
    std::vector<ComparePresentationCollapsedRunState>& collapsed_runs,
    std::size_t run_start_model_row,
    std::size_t run_length);
const ComparePresentationCollapsedRunState* FindCollapsedRunState(
    const std::vector<ComparePresentationCollapsedRunState>& collapsed_runs,
    std::size_t run_start_model_row,
    std::size_t run_length);

struct ComparePresentationOptions {
  bool show_whitespace = false;
  std::size_t context_lines = 3;
  std::size_t collapse_threshold = 8;
  // Review notes shown in the diff: `(*hunk_markers)[h].has_note` puts a
  // ReviewNote row after hunk h's last row; a non-empty `file_note` and every
  // detached note put one at the top. Borrowed for the build only.
  const std::vector<BranchReviewHunkMarker>* hunk_markers = nullptr;
  std::string_view file_note;
  const std::vector<BranchReviewNote>* detached_notes = nullptr;
};

struct ComparePresentationModel {
  std::vector<ComparePresentationRow> rows;
  ComparePresentationCollapseState collapse_state;
  // Back buffer for `collapse_state.collapsed_runs`. A rebuild has to read the
  // previous build's per-run expand state while writing the new list, and the
  // two are swapped rather than copied. Not part of the model's value — nothing
  // outside `BuildComparePresentationModelInto` should read it.
  std::vector<ComparePresentationCollapsedRunState> previous_collapsed_runs;
};

// Rebuild `presentation` IN PLACE from `model`, recycling its row slab, its
// summary strings and its collapsed-run buffer. The
// incoming `presentation.collapse_state` is read as the previous state.
//
// This is the form the product uses, because the rebuild fires on every
// keystroke in an editable diff and almost never changes the row count. The
// value-returning form below is an adapter over it for callers starting from
// nothing (TD-2026-08-17-261).
void BuildComparePresentationModelInto(ComparePresentationModel& presentation,
                                       const CompareModel& model,
                                       const CompareSemanticFileMetadata& semantic,
                                       const ComparePresentationOptions& options);

ComparePresentationModel BuildComparePresentationModel(
    const CompareModel& model,
    const CompareSemanticFileMetadata& semantic,
    const ComparePresentationOptions& options,
    ComparePresentationCollapseState collapse_state);

// Recompute `row.display_summary_text` from `summary_text`,
// `review_marker_label`, and `has_review_note`. Call after mutating any of
// those fields.
void ComposeComparePresentationDisplaySummary(ComparePresentationRow& row);

// Map a model row to the presentation row that represents it, always returning a
// valid presentation-space index (0 when the presentation is empty). Unlike
// ComparePresentationModelRowIndex this resolves a model row hidden inside a
// collapsed run to that run's CollapsedContext placeholder, and otherwise falls
// back to the nearest anchored row at or above it — so a caller storing the
// result into a presentation-space field (selection/scroll) never lands a raw
// out-of-range model index there.
std::size_t ComparePresentationRowForModelRow(
    const ComparePresentationModel& presentation,
    std::size_t model_row_index);

std::size_t ComparePresentationToModelRow(const ComparePresentationModel& presentation,
                                          std::size_t presentation_row_index);

}  // namespace microide::compare
