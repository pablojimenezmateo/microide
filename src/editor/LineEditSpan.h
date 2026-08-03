#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace microide::editor {

// Folds a batch of line-range splices into the one window a stale per-line cache
// needs in order to resync incrementally.
//
// A derived per-line cache (fold indents, per-line bracket events) only has to
// recompute the lines an edit actually touched. But a consumer that refreshes
// once per frame does not see one edit -- it sees whatever batch of edits landed
// since it last looked. This accumulates that batch into a single window with the
// two properties such a cache needs:
//
//   document[0, begin)              == cache[0, begin)              (common prefix)
//   document[current_end, doc_size) == cache[cached_end, cache_size) (common suffix)
//
// so the consumer recomputes `[begin, current_end)` and splices its tail across.
// The window is conservative -- it may cover lines that did not actually change --
// but it never omits one that did, so acting on it is always correct.
//
// `kToEnd` for either end means "through the end of that sequence": the common
// suffix is empty. That is what an edit of unknown extent degrades to, and it
// keeps a caller that only knows "everything from line N is suspect" expressible
// without a second code path.
class LineEditSpan {
 public:
  static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t kToEnd = std::numeric_limits<std::size_t>::max();

  // True when no edit has been recorded since the last Clear().
  bool empty() const noexcept { return begin_ == kNone; }

  // First line that may differ between the cache and the document.
  std::size_t begin() const noexcept { return begin_; }
  // Exclusive end of the changed window in the STALE cache, or kToEnd.
  std::size_t cached_end() const noexcept { return cached_end_; }
  // Exclusive end of the changed window in the CURRENT document, or kToEnd.
  std::size_t current_end() const noexcept { return current_end_; }

  void Clear() noexcept {
    begin_ = kNone;
    cached_end_ = 0;
    current_end_ = 0;
  }

  // Record that lines [start, start + removed) were replaced by `inserted` lines.
  // Coordinates are in the document as it stood immediately before this splice.
  void NoteSplice(std::size_t start, std::size_t removed, std::size_t inserted) noexcept {
    if (begin_ == kNone) {
      begin_ = start;
      cached_end_ = start + removed;
      current_end_ = start + inserted;
      return;
    }
    begin_ = std::min(begin_, start);
    if (current_end_ == kToEnd) {
      // Already extends past everything this splice could touch; only the window
      // start can still move.
      return;
    }
    const std::size_t splice_end = start + removed;
    if (splice_end > current_end_) {
      // The splice reaches past the window into what was still common suffix.
      // Those lines map one-for-one onto the cache's suffix, so the cache-side
      // end advances by the same count.
      cached_end_ += splice_end - current_end_;
      current_end_ = splice_end;
    }
    // Everything at/after the splice shifts by (inserted - removed), and the
    // window end is at/after it, so it shifts too. `current_end_ >= removed`
    // holds because current_end_ >= start + removed, keeping this unsigned-safe.
    current_end_ = current_end_ - removed + inserted;
  }

  // Record that every line from `start` on may differ, with no reusable common
  // suffix. This is what an edit whose extent is not reported degrades to.
  void NoteSuffixReplaced(std::size_t start) noexcept {
    begin_ = begin_ == kNone ? start : std::min(begin_, start);
    cached_end_ = kToEnd;
    current_end_ = kToEnd;
  }

  // Nothing of the cache is reusable.
  void MarkFullRebuild() noexcept { NoteSuffixReplaced(0); }

  // "Everything from `start` on may differ" as a value, for callers that know
  // only where a change began.
  static LineEditSpan SuffixReplacedFrom(std::size_t start) noexcept {
    LineEditSpan span;
    span.NoteSuffixReplaced(start);
    return span;
  }
  static LineEditSpan FullRebuild() noexcept { return SuffixReplacedFrom(0); }

  // Resolve `cached_end()` against a cache holding `cache_line_count` entries.
  std::size_t ResolvedCachedEnd(std::size_t cache_line_count) const noexcept {
    return cached_end_ == kToEnd ? cache_line_count : std::min(cached_end_, cache_line_count);
  }
  // Resolve `current_end()` against a document holding `document_line_count` lines.
  std::size_t ResolvedCurrentEnd(std::size_t document_line_count) const noexcept {
    return current_end_ == kToEnd ? document_line_count
                                  : std::min(current_end_, document_line_count);
  }

 private:
  std::size_t begin_ = kNone;
  std::size_t cached_end_ = 0;
  std::size_t current_end_ = 0;
};

}  // namespace microide::editor
