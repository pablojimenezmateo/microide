#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compare/CompareReviewTypes.h"

namespace microide::compare {

using CompareTextBuffer = std::shared_ptr<const std::string>;

const CompareTextBuffer& EmptyCompareText();

CompareTextBuffer MakeCompareText(std::string text);

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
  std::string_view left_text;
  std::string_view right_text;
  int left_line = 0;
  int right_line = 0;
  CompareRowKind kind = CompareRowKind::Unchanged;
  int hunk = -1;
  std::vector<CompareTextSpan> left_changed_spans;
  std::vector<CompareTextSpan> right_changed_spans;
  bool left_moved = false;
  bool right_moved = false;
};

enum class CompareSideChange {
  None,
  Novel,
  Moved,
  LayoutOnly,
};

CompareSideChange LeftSideChange(const CompareRow& row);
CompareSideChange RightSideChange(const CompareRow& row);
bool CompareRowIsLayoutOnly(const CompareRow& row);

struct CompareHunk {
  int index = 0;
  int start_row = 0;
  int end_row = 0;
};

namespace detail {
struct CompareBuildScratch;
struct DiffScratch;
void DestroyCompareBuildScratch(CompareBuildScratch* scratch) noexcept;
}

class CompareBuildScratchHandle {
 public:
  CompareBuildScratchHandle() = default;
  ~CompareBuildScratchHandle() { detail::DestroyCompareBuildScratch(scratch_); }
  CompareBuildScratchHandle(const CompareBuildScratchHandle&) {}
  CompareBuildScratchHandle& operator=(const CompareBuildScratchHandle&) { return *this; }
  CompareBuildScratchHandle(CompareBuildScratchHandle&& other) noexcept
      : scratch_(std::exchange(other.scratch_, nullptr)) {}
  CompareBuildScratchHandle& operator=(CompareBuildScratchHandle&& other) noexcept {
    std::swap(scratch_, other.scratch_);
    return *this;
  }

  detail::CompareBuildScratch* get() const noexcept { return scratch_; }
  void reset(detail::CompareBuildScratch* scratch) noexcept {
    detail::DestroyCompareBuildScratch(scratch_);
    scratch_ = scratch;
  }

 private:
  detail::CompareBuildScratch* scratch_ = nullptr;
};

struct CompareModel {
  CompareTextBuffer left_source = EmptyCompareText();
  CompareTextBuffer right_source = EmptyCompareText();
  std::vector<CompareRow> rows;
  std::vector<CompareHunk> hunks;
  bool left_final_newline_missing = false;
  bool right_final_newline_missing = false;
  bool left_absent = false;
  bool right_absent = false;
  bool left_uses_crlf = false;
  bool right_uses_crlf = false;
  CompareBuildScratchHandle build_scratch;
};

struct CompareBuildProfile {
  std::uint64_t split_lines_ns = 0;
  std::uint64_t line_alignment_ns = 0;
  std::uint64_t hunk_refinement_ns = 0;
  std::uint64_t row_assembly_ns = 0;
  std::uint64_t total_ns = 0;
  std::size_t exact_line_alignment_calls = 0;
  std::size_t anchored_line_alignment_calls = 0;
  std::size_t structural_hunks = 0;
  std::size_t flat_fallback_hunks = 0;
  std::size_t graph_vertices = 0;
  std::size_t cached_hunks = 0;
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
  std::string_view text;
  std::string_view right_text;
};

struct LineDiffBuildStats {
  std::size_t exact_alignment_calls = 0;
  std::size_t anchored_alignment_calls = 0;
};

CompareModel BuildCompareModel(const std::string& left, const std::string& right);
CompareModel BuildCompareModel(const std::string& left,
                               const std::string& right,
                               const CompareBuildOptions& options);
CompareBuildProfile BuildCompareModelInto(CompareModel& model,
                                          CompareTextBuffer left,
                                          CompareTextBuffer right,
                                          const CompareBuildOptions& options);
CompareBuildProfile BuildCompareModelInto(CompareModel& model,
                                          const std::string& left,
                                          const std::string& right,
                                          const CompareBuildOptions& options);

CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right);
CompareBuildResult BuildCompareModelProfiled(const std::string& left,
                                             const std::string& right,
                                             const CompareBuildOptions& options);
std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     LineDiffBuildStats* stats = nullptr);
std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     const CompareBuildOptions& options,
                                     LineDiffBuildStats* stats = nullptr);
void BuildLineDiffOpsInto(std::span<const std::string_view> left_lines,
                          std::span<const std::string_view> right_lines,
                          const CompareBuildOptions& options,
                          detail::DiffScratch& scratch,
                          std::vector<DiffOp>& ops,
                          LineDiffBuildStats* stats = nullptr);

}
