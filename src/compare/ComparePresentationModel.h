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
  int hunk_index = -1;
  int collapsed_line_count = 0;
  bool context_above = false;
};

struct ComparePresentationCollapseState {
  std::vector<bool> expanded_above;
  std::vector<bool> expanded_below;
};

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
