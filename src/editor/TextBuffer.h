#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/PieceTree.h"

namespace microide::editor {

// Line-oriented document storage behind a stable interface.
//
// Phase 2 of the large-file overhaul introduced this seam; Phase 3 swaps the
// underlying representation to a piece tree (see PieceTree) without touching the
// edit engine, undo history, or the read-only consumers.
//
// Representation:
//   * `tree_` is the authoritative store -- a piece tree over an immutable
//     original buffer plus an append-only add buffer. Edits are O(log n) splices
//     and never reshuffle line storage; there is no per-line heap allocation.
//   * `LineView` reads a single line zero-copy (the common case) and is the
//     accessor hot/per-line code must use.
//
// Compatibility accessors returning `const std::string&`:
//   * `operator[]`, `front`, and `back` return a reference into a per-line
//     materialization cache (`line_cache_`): the requested line is copied out of
//     the tree once and memoized, so repeated access and `&buffer[i]` pointers
//     stay valid. Only *accessed* lines are materialized -- never the whole
//     document -- so the render path (which touches one viewport's worth of
//     lines) does not pay an O(n) rebuild per frame or per keystroke. New
//     per-line code should still prefer `LineView` (zero-copy view).
//   * `Snapshot()` and the iterators return a lazily materialized whole-document
//     `snapshot_` vector for inherently O(n) cold paths (serialize, persist,
//     filetype detect, full-buffer search, range scans). Never call these on a
//     hot/per-frame path.
//   * Every reference returned by the above is invalidated by the next mutation
//     -- the same contract `std::vector<std::string>` already imposed.
//   * All mutation funnels through `ReplaceLineRange`, which maps directly onto a
//     piece-tree offset-range splice; the convenience mutators wrap it.
class TextBuffer {
 public:
  // Iterates the materialized snapshot (yields `const std::string&`). Building
  // the snapshot is O(n); prefer index + LineView in per-line code.
  using const_iterator = std::vector<std::string>::const_iterator;

  TextBuffer() = default;
  explicit TextBuffer(const std::vector<std::string>& lines) : tree_(lines) {}

  // Replace the entire contents (used by load / ResetState).
  void Reset(const std::vector<std::string>& lines) {
    tree_.Reset(lines);
    InvalidateSnapshot();
  }

  // Large-file load fast path: take ownership of canonical '\n'-joined `content`
  // directly as the backing buffer (no per-line split/rejoin). `content` must
  // not contain '\r'. See PieceTree::ResetFromText.
  void ResetFromText(std::string content) {
    tree_.ResetFromText(std::move(content));
    InvalidateSnapshot();
  }

  // --- Read ---
  std::size_t LineCount() const noexcept { return tree_.LineCount(); }
  std::size_t size() const noexcept { return tree_.LineCount(); }
  bool empty() const noexcept { return tree_.Empty(); }

  // Zero-copy view of line `index` (no trailing newline). `index` must be < size().
  // Prefer this in hot/per-line code; it is the accessor the piece tree keeps.
  std::string_view LineView(std::size_t index) const { return tree_.LineView(index); }
  std::size_t LineLength(std::size_t index) const { return tree_.LineLength(index); }

  // Per-line compatibility accessors (line-cache backed; see class comment).
  // Materialize only the requested line, not the whole document.
  const std::string& operator[](std::size_t index) const { return LineRef(index); }
  const std::string& front() const { return LineRef(0); }
  const std::string& back() const { return LineRef(tree_.LineCount() - 1); }
  // Iterators materialize the whole document (cold paths only).
  const_iterator begin() const { return Snapshot().begin(); }
  const_iterator end() const { return Snapshot().end(); }

  // Copy lines [begin, end) into a fresh vector (undo before/after capture).
  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const {
    return tree_.SliceLines(begin, end);
  }
  // Full materialized copy of the document.
  std::vector<std::string> ToVector() const { return tree_.ToVector(); }

  // The whole document as one '\n'-joined string, appended to `out`.
  //
  // This is what the piece tree already stores, so it is a single tree walk with
  // one memcpy per piece. Building the same string by looping LineView costs a
  // pair of binary searches per line AND, on a heavily edited buffer, populates
  // the piece tree's per-line materialization cache with a second full copy of
  // the document that is retained until the next mutation.
  void AppendWholeText(std::string& out) const {
    tree_.AppendWholeText(out);
  }

  // Whole-document bridge for inherently O(n) cold paths (see class comment).
  // Prefer LineView for anything per-line or hot.
  const std::vector<std::string>& Snapshot() const {
    if (!snapshot_valid_) {
      ++s_snapshot_builds_;
      snapshot_ = tree_.ToVector();
      snapshot_valid_ = true;
    }
    return snapshot_;
  }

  // Test-only: process-wide count of full-document Snapshot() materializations.
  // Lets regression tests assert that hot edit paths (e.g. grouped/multi-caret
  // undo) never fall back to whole-buffer copies. Cold-path increment only.
  static std::size_t snapshot_build_count() { return s_snapshot_builds_; }
  static void reset_snapshot_build_count() { s_snapshot_builds_ = 0; }

  // Number of lines currently materialized by the `operator[]`/front/back
  // compatibility accessors. Every entry is a full heap copy of a line that the
  // piece tree already stores, retained until the next mutation — so a per-line
  // hot path that reaches for `operator[]` instead of `LineView` quietly
  // accumulates a second copy of every line it touches. Exposed so a test can
  // pin "this render path stays zero-copy" rather than trusting the convention.
  std::size_t materialized_line_count() const { return line_cache_.size(); }

  // --- Mutate ---
  // Universal primitive: erase `removed` lines starting at `start`, then insert
  // `inserted` at `start`. Mirrors the undo-entry apply model.
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted) {
    tree_.ReplaceLineRange(start, removed, inserted);
    InvalidateSnapshot();
  }

  // Column-scoped splice: replace the bytes between (start_line, start_column)
  // and (end_line, end_column) with `text`. The primitive an in-line edit wants —
  // it copies only `text`, where the line-shaped `ReplaceLineRange` copies the
  // whole affected line twice (see PieceTree::ReplaceTextRange).
  void ReplaceTextRange(std::size_t start_line, std::size_t start_column,
                        std::size_t end_line, std::size_t end_column,
                        std::string_view text) {
    tree_.ReplaceTextRange(start_line, start_column, end_line, end_column, text);
    InvalidateSnapshot();
  }

  // Append the bytes in [(start_line, start_column), (end_line, end_column)) to
  // `out`. O(range); never materializes a whole line the way LineView does for a
  // line that spans pieces.
  void AppendTextRange(std::size_t start_line, std::size_t start_column,
                       std::size_t end_line, std::size_t end_column,
                       std::string& out) const {
    tree_.AppendTextRange(start_line, start_column, end_line, end_column, out);
  }

  void SetLine(std::size_t index, const std::string& value) {
    tree_.SetLine(index, value);
    InvalidateSnapshot();
  }
  void InsertLine(std::size_t index, const std::string& value) {
    tree_.InsertLine(index, value);
    InvalidateSnapshot();
  }
  void EraseLine(std::size_t index) {
    tree_.EraseLine(index);
    InvalidateSnapshot();
  }
  void EraseLineRange(std::size_t begin, std::size_t end) {
    tree_.EraseLineRange(begin, end);
    InvalidateSnapshot();
  }
  void PushBackLine(const std::string& value) {
    tree_.PushBackLine(value);
    InvalidateSnapshot();
  }

 private:
  // Memoized single-line materialization. unordered_map node values are
  // reference-stable across inserts/rehashes, so `&LineRef(i)` stays valid until
  // the next mutation clears the cache.
  const std::string& LineRef(std::size_t index) const {
    const auto it = line_cache_.find(index);
    if (it != line_cache_.end()) return it->second;
    return line_cache_.emplace(index, std::string(tree_.LineView(index))).first->second;
  }

  void InvalidateSnapshot() {
    snapshot_valid_ = false;
    snapshot_.clear();
    line_cache_.clear();
  }

  PieceTree tree_;
  mutable std::vector<std::string> snapshot_;
  mutable bool snapshot_valid_ = false;
  mutable std::unordered_map<std::size_t, std::string> line_cache_;
  inline static std::size_t s_snapshot_builds_ = 0;
};

}  // namespace microide::editor
