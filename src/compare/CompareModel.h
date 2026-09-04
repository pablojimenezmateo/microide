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

// Shared, immutable document text that compare rows view into.
//
// Immutable and shared rather than owned outright so the same bytes can back a
// tab's read-only left buffer, its model, and any copy of that model, without a
// copy at any of those boundaries — and so the string OBJECT's address is fixed
// for the buffer's whole life, which is what makes a `std::string_view` row safe
// across a model copy or move.
using CompareTextBuffer = std::shared_ptr<const std::string>;

// A shared empty buffer, so "no text" needs neither a null check nor its own
// allocation. One object process-wide.
const CompareTextBuffer& EmptyCompareText();

// `text` as a shared buffer, taking ownership of the bytes.
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
  // Views into the owning CompareModel's `left_source`/`right_source` (see there).
  // Valid for as long as that model holds those buffers unchanged — i.e. until the
  // next build into the same model, which is the same invalidation point the row
  // vector itself has always had.
  //
  // A row built by hand (tests, fixtures) may instead view any storage that
  // outlives it; the model does not assume its rows point into its own buffers.
  // A string LITERAL is fine — static storage. A `std::string` local or parameter
  // is not: assigning one here compiles silently and dangles the moment it goes
  // out of scope. Put such text in the model's `left_source`/`right_source` (see
  // `MakeCompareText`) and view that, which is what the builder does.
  std::string_view left_text;
  std::string_view right_text;
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

// Working storage for one in-place model rebuild, kept alive between rebuilds.
//
// The build's three biggest buffers — the two `SplitLineViews` line lists and
// the aligner's `DiffOp` output, plus the recursion scratch behind them — were
// all locals, so an editable compare pane allocated and freed roughly 700 KB of
// them PER KEYSTROKE. Every one is dead by the time a build returns and identical
// in shape to the next build's, which is exactly what makes retaining them
// correct (TD-2026-08-17-261).
//
// Opaque on purpose: the aligner's scratch is an implementation detail of
// `CompareModel.cpp` and nothing outside it may name the members.
namespace detail {
struct CompareBuildScratch;
struct DiffScratch;
void DestroyCompareBuildScratch(CompareBuildScratch* scratch) noexcept;
}  // namespace detail

// Owning handle with value semantics chosen for what the scratch IS. Copying a
// model must not copy megabytes of dead working buffers, and two models must
// never share one — so a copy starts empty, and a move transfers.
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
  // The bytes every row's `left_text`/`right_text` view points into.
  //
  // A CompareRow used to own two std::strings, so the cold open of a ~24,000-row
  // diff cost 48,000 allocations of ~31 bytes — the top three allocation sites of
  // `diff.open_large_compare` (TD-2026-08-14-232). Every row's text is an exact
  // slice of one of the two inputs, so the model keeps one copy of each input and
  // the rows view into it: two allocations for a whole build instead of two per
  // row, and none at all on a rebuild that can reuse the buffers.
  //
  // Owned rather than borrowed, because the builder's inputs are routinely
  // temporaries — the editable right pane is serialized fresh on every rebuild —
  // so there is nothing stable to borrow from.
  //
  // Held as a shared buffer specifically so the model stays copyable and movable
  // with no rebasing: the string OBJECT's address is what the views depend on, and
  // a shared_ptr keeps it fixed through any number of copies. A plain
  // `std::string` member would relocate its bytes on a move whenever they were
  // small enough for SSO, silently dangling every row view — and
  // `BuildCompareModel` returns by value, so that move is on the ordinary path,
  // not a corner case.
  //
  // Sharing also means the read-only left side is never copied at all: the tab
  // holds the same buffer it hands the builder, so a rebuild (one per keystroke in
  // an editable pane) does not memcpy the left file.
  CompareTextBuffer left_source = EmptyCompareText();
  CompareTextBuffer right_source = EmptyCompareText();
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
  // The side's file does not exist (see CompareBuildOptions::left_exists): a
  // whole-file creation or deletion rather than an edit down to zero lines.
  bool left_absent = false;
  bool right_absent = false;
  // True when the corresponding source buffer uses CRLF line terminators.
  // SplitLineViews strips the ending, so a CRLF file's rows carry bare text —
  // but git stores the `\r` as part of each line's content, so a generated patch
  // must re-emit `text\r\n` for its context/`-`/`+` body lines to byte-match the
  // CRLF blob under `git apply`. Without this, staging/discarding a hunk of a
  // CRLF working-tree file fails context matching (fails safe: patch rejected).
  bool left_uses_crlf = false;
  bool right_uses_crlf = false;
  // Rebuild working storage, NOT part of the model's value. Lazily created by
  // the first in-place rebuild and reused by every later one; empty in a copy.
  CompareBuildScratchHandle build_scratch;
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
// Rebuild `model` in place, ADOPTING the two buffers and RECYCLING the existing
// row storage. This is the primary form: nothing is copied on either side.
//
// The rows of consecutive rebuilds are nearly identical, so reusing the previous
// build's row objects keeps a rebuild's cost near zero without changing what is
// produced: every field of a recycled row is reset, and rows past the new end are
// dropped (TD-2026-08-13-208). Since TD-2026-08-14-232 a row's text is a view into
// `model.left_source`/`right_source` rather than two owned strings, so a build no
// longer allocates per row at all.
//
// A null buffer is read as an empty document.
void BuildCompareModelInto(CompareModel& model,
                           CompareTextBuffer left,
                           CompareTextBuffer right,
                           const CompareBuildOptions& options);
// Copying form, for callers holding plain strings (tests, one-shot builds). Each
// input is copied into a fresh shared buffer the model then owns.
void BuildCompareModelInto(CompareModel& model,
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
// Into-form taking the aligner's recursion scratch AND its output vector from
// the caller, so both survive between rebuilds. `ops` is cleared, not appended
// to. `scratch` is opaque here on purpose — only `CompareModel.cpp` and the
// model's own `build_scratch` produce one.
void BuildLineDiffOpsInto(std::span<const std::string_view> left_lines,
                          std::span<const std::string_view> right_lines,
                          const CompareBuildOptions& options,
                          detail::DiffScratch& scratch,
                          std::vector<DiffOp>& ops,
                          LineDiffBuildStats* stats = nullptr);

}  // namespace microide::compare
