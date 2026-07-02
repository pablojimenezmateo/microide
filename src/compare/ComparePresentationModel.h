#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"

namespace microide::compare {

enum class ComparePresentationRowKind {
  Model,
  Metadata,
  CollapsedContext,
  HunkHeader,
};

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
};

struct CompareInlineDiffCache {
  std::uint64_t model_generation = 0;
  std::vector<std::vector<CompareTextSpan>> left_spans_by_row;
  std::vector<std::vector<CompareTextSpan>> right_spans_by_row;
};

struct ComparePresentationModel {
  std::vector<ComparePresentationRow> rows;
  ComparePresentationCollapseState collapse_state;
  CompareInlineDiffCache inline_cache;
};

ComparePresentationModel BuildComparePresentationModel(
    const CompareModel& model,
    const CompareSemanticFileMetadata& semantic,
    const ComparePresentationOptions& options,
    ComparePresentationCollapseState collapse_state,
    std::uint64_t model_generation);

// Recompute `row.display_summary_text` from `summary_text`,
// `review_marker_label`, and `has_review_note`. Call after mutating any of
// those fields.
void ComposeComparePresentationDisplaySummary(ComparePresentationRow& row);

std::optional<std::size_t> ComparePresentationModelRowIndex(
    const ComparePresentationModel& presentation,
    std::size_t model_row_index);

std::size_t ComparePresentationToModelRow(const ComparePresentationModel& presentation,
                                          std::size_t presentation_row_index);

void RebuildCompareInlineDiffCache(ComparePresentationModel* presentation,
                                   const CompareModel& model,
                                   std::uint64_t model_generation,
                                   bool defer_intraline);

const std::vector<CompareTextSpan>& CompareInlineLeftSpans(
    const ComparePresentationModel& presentation,
    const CompareModel& model,
    std::size_t model_row_index);

const std::vector<CompareTextSpan>& CompareInlineRightSpans(
    const ComparePresentationModel& presentation,
    const CompareModel& model,
    std::size_t model_row_index);

}  // namespace microide::compare
