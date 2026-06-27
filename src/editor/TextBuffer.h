#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::editor {

// Line-oriented document storage behind a stable interface.
//
// Phase 2 of the large-file overhaul introduces this seam so the underlying
// representation can be swapped (Phase 3: a piece tree) without touching the
// edit engine, undo history, or the ~30 read-only consumers. The current
// implementation is a thin wrapper over `std::vector<std::string>`, so it is
// behavior- and performance-identical to the previous raw member.
//
// Design rules that keep the door open for the piece-tree swap:
//   * Line reads return `std::string_view` (zero-copy for the vector impl; a
//     piece tree materializes only when a logical line spans a piece boundary).
//   * All mutation funnels through `ReplaceLineRange` — "replace `removed`
//     whole lines starting at `start` with `inserted`". This is exactly the
//     undo-entry model and maps directly onto an offset-range splice in a piece
//     tree. The convenience mutators are thin wrappers over it.
//   * `Snapshot()` is the explicit "I need the whole document as a vector"
//     bridge for inherently whole-document cold paths (serialize, persist,
//     filetype detect, full-buffer search). The piece-tree impl will satisfy it
//     via revision-cached materialization; hot paths must use `LineView`.
class TextBuffer {
 public:
  // Phase 2 keeps a vector-backed iterator (yields `const std::string&`) so the
  // ~30 read-only consumers compile unchanged. Phase 3 replaces this with a
  // representation-agnostic line iterator yielding `std::string_view`, at which
  // point the compiler flags every consumer that must move to LineView.
  using const_iterator = std::vector<std::string>::const_iterator;

  TextBuffer() = default;
  explicit TextBuffer(std::vector<std::string> lines) : lines_(std::move(lines)) {}

  // Replace the entire contents (used by load / ResetState).
  void Reset(std::vector<std::string> lines) { lines_ = std::move(lines); }

  // --- Read ---
  std::size_t LineCount() const noexcept { return lines_.size(); }
  std::size_t size() const noexcept { return lines_.size(); }
  bool empty() const noexcept { return lines_.empty(); }

  // Zero-copy view of line `index` (no trailing newline). `index` must be < size().
  // Prefer this in hot/per-line code; it is the accessor the piece tree keeps.
  std::string_view LineView(std::size_t index) const { return lines_[index]; }
  // Vector-impl line access kept as `const std::string&` for Phase 2 so reader
  // call sites are behavior-identical. Phase 3 narrows this to LineView.
  const std::string& operator[](std::size_t index) const { return lines_[index]; }
  std::size_t LineLength(std::size_t index) const { return lines_[index].size(); }

  const_iterator begin() const { return lines_.begin(); }
  const_iterator end() const { return lines_.end(); }
  const std::string& front() const { return lines_.front(); }
  const std::string& back() const { return lines_.back(); }

  // Copy lines [begin, end) into a fresh vector (undo before/after capture).
  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const;
  // Full materialized copy of the document.
  std::vector<std::string> ToVector() const { return lines_; }

  // Whole-document bridge for inherently O(n) cold paths (see class comment).
  // Prefer LineView for anything per-line or hot.
  const std::vector<std::string>& Snapshot() const { return lines_; }

  // --- Mutate ---
  // Universal primitive: erase `removed` lines starting at `start`, then insert
  // `inserted` at `start`. Mirrors the undo-entry apply model.
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted);

  void SetLine(std::size_t index, std::string value) { lines_[index] = std::move(value); }
  void InsertLine(std::size_t index, std::string value);
  void EraseLine(std::size_t index);
  void EraseLineRange(std::size_t begin, std::size_t end);
  void PushBackLine(std::string value) { lines_.push_back(std::move(value)); }

  // Direct mutable access to a single line for in-place edits in the hot edit
  // path. The vector impl returns a real reference; the piece-tree impl will
  // route per-line edits through ReplaceLineRange instead, so new code should
  // prefer SetLine + LineView. Kept narrow on purpose.
  std::string& MutableLine(std::size_t index) { return lines_[index]; }

 private:
  std::vector<std::string> lines_;
};

}  // namespace microide::editor
