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
  ReviewNote,
};

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
  const std::vector<BranchReviewHunkMarker>* hunk_markers = nullptr;
  std::string_view file_note;
  const std::vector<BranchReviewNote>* detached_notes = nullptr;
};

struct ComparePresentationModel {
  std::vector<ComparePresentationRow> rows;
  ComparePresentationCollapseState collapse_state;
  std::vector<ComparePresentationCollapsedRunState> previous_collapsed_runs;
};

void BuildComparePresentationModelInto(ComparePresentationModel& presentation,
                                       const CompareModel& model,
                                       const CompareSemanticFileMetadata& semantic,
                                       const ComparePresentationOptions& options);

ComparePresentationModel BuildComparePresentationModel(
    const CompareModel& model,
    const CompareSemanticFileMetadata& semantic,
    const ComparePresentationOptions& options,
    ComparePresentationCollapseState collapse_state);

void ComposeComparePresentationDisplaySummary(ComparePresentationRow& row);

std::size_t ComparePresentationRowForModelRow(
    const ComparePresentationModel& presentation,
    std::size_t model_row_index);

std::size_t ComparePresentationToModelRow(const ComparePresentationModel& presentation,
                                          std::size_t presentation_row_index);

}
