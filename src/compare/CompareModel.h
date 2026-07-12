#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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
  // True when the corresponding source buffer was non-empty and did NOT end with
  // a newline. Lets the patch generator emit git's `\ No newline at end of file`
  // marker so staging/discarding a hunk that touches the final line does not
  // silently add a trailing newline.
  bool left_final_newline_missing = false;
  bool right_final_newline_missing = false;
  // True when the corresponding source buffer was empty. Distinguishes a
  // whole-file add (left empty) or delete (right empty) — which the diff can't
  // otherwise tell from a hunk that merely happens to be all additions/deletions
  // — so the patch generator can emit `/dev/null` headers.
  bool left_empty = false;
  bool right_empty = false;
  // True when the corresponding source buffer uses CRLF line terminators.
  // SplitLineViews strips the ending, so a CRLF file's rows carry bare text —
  // but git stores the `\r` as part of each line's content, so a generated patch
  // must re-emit `text\r\n` for its context/`-`/`+` body lines to byte-match the
  // CRLF blob under `git apply`. Without this, staging/discarding a hunk of a
  // CRLF working-tree file fails context matching (fails safe: patch rejected).
  bool left_uses_crlf = false;
  bool right_uses_crlf = false;
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
  // only while those buffers outlive the returned ops. For Delete this is the
  // left line; for Insert the right line; for Equal the left line.
  std::string_view text;
  // For Equal ops the matched right line. Under ignore_whitespace the two sides
  // can be considered equal while differing in whitespace, so the right column
  // must reproduce the right file's text rather than a copy of `text`. Unused
  // (empty) for Delete/Insert.
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

}  // namespace microide::compare
