#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareReviewTypes.h"

namespace microide::compare {

enum class CompareRowKind {
  Unchanged,
  Added,
  Deleted,
  Modified,
};

struct CompareTextSpan {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct CompareRow {
  std::string left_text;
  std::string right_text;
  int left_line = 0;
  int right_line = 0;
  CompareRowKind kind = CompareRowKind::Unchanged;
  int hunk = -1;
  std::vector<CompareTextSpan> left_changed_spans;
  std::vector<CompareTextSpan> right_changed_spans;
};

struct CompareHunk {
  int index = 0;
  int start_row = 0;
  int end_row = 0;
};

struct CompareModel {
  std::vector<CompareRow> rows;
  std::vector<CompareHunk> hunks;
};

struct CompareBuildProfile {
  std::uint64_t split_lines_ns = 0;
  std::uint64_t line_alignment_ns = 0;
  std::uint64_t hunk_alignment_ns = 0;
  std::uint64_t intraline_ns = 0;
  std::uint64_t row_assembly_ns = 0;
  std::uint64_t total_ns = 0;
  std::size_t exact_line_alignment_calls = 0;
  std::size_t anchored_line_alignment_calls = 0;
  std::size_t exact_hunk_alignment_calls = 0;
  std::size_t fallback_hunk_alignment_calls = 0;
  std::size_t token_intraline_calls = 0;
  std::size_t codepoint_intraline_calls = 0;
};

struct CompareBuildResult {
  CompareModel model;
  CompareBuildProfile profile;
};

enum class DiffOpKind {
  Equal,
  Delete,
  Insert,
};

struct DiffOp {
  DiffOpKind kind = DiffOpKind::Equal;
  // View into the caller-owned source buffers passed to the diff routine. Valid
  // only while those buffers outlive the returned ops.
  std::string_view text;
};

struct LineDiffBuildStats {
  std::size_t exact_alignment_calls = 0;
  std::size_t anchored_alignment_calls = 0;
};

CompareModel BuildCompareModel(const std::string& left, const std::string& right);
CompareModel BuildCompareModel(const std::string& left,
                               const std::string& right,
                               const CompareBuildOptions& options);
CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right);
CompareBuildResult BuildCompareModelProfiled(const std::string& left,
                                             const std::string& right,
                                             const CompareBuildOptions& options);
std::vector<DiffOp> BuildLineDiffOps(const std::vector<std::string_view>& left_lines,
                                     const std::vector<std::string_view>& right_lines,
                                     LineDiffBuildStats* stats = nullptr);
std::vector<DiffOp> BuildLineDiffOps(const std::vector<std::string_view>& left_lines,
                                     const std::vector<std::string_view>& right_lines,
                                     const CompareBuildOptions& options,
                                     LineDiffBuildStats* stats = nullptr);

}  // namespace microide::compare
