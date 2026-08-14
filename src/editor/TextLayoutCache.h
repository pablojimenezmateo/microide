#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/TextLayout.h"

namespace microide::editor {

class FoldingModel;

// An edit confined to a single line, described by the text it spliced in.
//
// A line already known to be plain ASCII stays plain exactly when the text
// spliced into it is, and a plain line's visual width is its byte count. So with
// this `TextLayoutCache::UpdateVisualColumnCacheAfterEdit` updates the width table
// in O(inserted) instead of re-measuring the whole line -- which on a line with no
// newlines in it is a megabyte scan per keystroke (TD-2026-08-05-132, item 2).
struct InlineLineSplice {
  std::string_view inserted_text;
  bool valid = false;
};

// Owns the per-viewport derived-layout caches that previously lived as flat
// mutable members on TextViewport: the visible-line LayoutLine LRU, the
// wrapped-row layout vector (soft-wrap / fold-aware visual-row mapping), and
// the visual-column-width cache backing MaxVisualColumns / scrollbar math.
//
// Methods take the inputs they need by value or const ref — the cache does
// not hold a back-pointer to the owning viewport, so the indirection from
// TextViewport into the cache is a plain non-virtual member call without
// any added allocations. This mirrors the rule the doc lifts from the
// rejected TextDocumentModel experiment: "no shared ownership for hot
// editor state unless proven free", "no full-buffer copies in
// render/layout/scroll/typing paths".
class TextLayoutCache {
 public:
  struct WrappedRow {
    std::size_t line_index = 0;
    std::size_t visual_start = 0;
    std::size_t visual_end = 0;
    // Hanging-indent: visual columns of leading whitespace the renderer should
    // prepend before this row's content. Zero for first rows of a line, for
    // the trivial path, and for the fold-but-no-wrap path.
    std::size_t indent = 0;
  };

  struct Stats {
    std::size_t visible_line_queries = 0;
    std::size_t visible_line_hits = 0;
    // Entries dropped by the LRU to stay under kVisibleLineCacheLimit.
    //
    // VisibleLineLayoutRefCached hands out a reference INTO this cache, so an
    // eviction is what could invalidate a reference a caller still holds. The
    // safety argument has always been "a frame's working set is far below the
    // limit, so a frame never evicts what it is still reading" — true, but until
    // now unmeasured, so nothing failed if someone lowered the limit or a caller
    // started querying many more lines (TD-2026-07-25-104). This counter is what
    // makes that argument testable.
    std::size_t visible_line_evictions = 0;
  };

  // ---- visible-line LayoutLine LRU --------------------------------------
  // Returns the LayoutLine for `line_index` from the cache, or builds it on
  // miss. Caller is responsible for caret-row decoration (the cache does not
  // know which row the cursor is on). Hands back the cached LayoutLine in place
  // instead of copying it out. The returned reference stays valid for the next
  // kVisibleLineCacheLimit queries: the order is a true LRU, so a query can only
  // recycle an entry that is older than every entry touched since — including
  // the ones a caller merely READ (a FIFO gave that guarantee only to entries the
  // frame had built, TD-2026-08-12-189). Like the by-value variant, the cache
  // does not set caret fields.
  //
  // `content_revision` is what lets a miss reuse the per-line width table this
  // cache already maintains, so building a row does not re-walk the line for its
  // width and its first visible cell.
  //
  // `facts_hint` is a fallback for callers that know the line's visual width from
  // somewhere other than this cache's width table -- the soft-wrap path knows it
  // from the wrapped-row table, whose last row for a line ENDS at that width.
  // Without it a wrapped row is built with unknown facts, which walks the whole
  // logical line to measure it: once per visible row, per frame, which on a file
  // with no line breaks in it is megabytes per row. Ignored when the width table
  // is current (that answer is strictly better -- it also knows plain-ASCII-ness).
  const LayoutLine& VisibleLineLayoutRefCached(LineSpan lines,
                                               std::size_t line_index,
                                               std::size_t horizontal_scroll,
                                               std::size_t visible_columns,
                                               std::size_t tab_size,
                                               std::uint64_t content_revision,
                                               LineLayoutFacts facts_hint = {}) const;

  // ---- wrapped row layout (soft-wrap + folds) ---------------------------
  // Rebuilds the wrapped-row table when its keyed inputs changed. The
  // trivial-layout fast path (no soft-wrap, no collapsed fold) leaves the
  // vectors empty and lets the readers synthesize rows from
  // `horizontal_scroll` + `visible_columns` directly.
  void EnsureWrappedRowLayouts(LineSpan lines,
                               std::size_t tab_size,
                               std::size_t visible_columns,
                               bool soft_wrap,
                               const FoldingModel* folding_model,
                               std::uint64_t layout_shape_revision,
                               std::uint64_t content_revision) const;

  // Incrementally re-wraps only the edited logical-line range and splices the
  // new rows into the wrapped-row table, keeping per-keystroke cost proportional
  // to the edit size instead of the whole document. Handles the pure soft-wrap
  // path (no collapsed folds); any other mode (trivial, fold-but-no-wrap,
  // collapsed folds, or a table that is out of sync / built for different keys)
  // returns false without touching the cache, so the content_revision guard in
  // EnsureWrappedRowLayouts safely triggers a full rebuild on next access.
  //
  // `inserted_count` is a COUNT, not the lines themselves: the new rows are
  // re-wrapped from the live buffer regardless, and an in-line edit's undo entry
  // no longer carries a materialized copy of the line it changed.
  bool UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                  std::size_t removed_count,
                                  std::size_t inserted_count,
                                  LineSpan lines,
                                  std::size_t tab_size,
                                  std::size_t visible_columns,
                                  bool soft_wrap,
                                  const FoldingModel* folding_model,
                                  std::uint64_t layout_shape_revision,
                                  std::uint64_t content_revision) const;

  // Trivial-mode-aware accessors. EnsureWrappedRowLayouts() must have been
  // called for the current input set before any of these are used. The
  // trivial and fold-but-no-wrap paths synthesize the row span from the
  // live `horizontal_scroll` / `visible_columns` passed here.
  WrappedRow WrappedRowAt(std::size_t visual_row_index,
                          std::size_t horizontal_scroll,
                          std::size_t visible_columns) const;
  std::size_t WrappedRowCount(std::size_t lines_size) const;
  std::size_t WrappedLineRowOffset(std::size_t line_index) const;
  // Inclusive [first_row, last_row] range of wrapped rows belonging to
  // `line_index`. Precondition: a non-trivial table with offsets is built
  // (callers check wrapped_row_layouts_trivial()/has_wrapped_line_row_offsets
  // first). Never returns last_row < first_row.
  std::pair<std::size_t, std::size_t> WrappedRowRangeForLine(std::size_t line_index,
                                                             std::size_t lines_size) const;
  // Index of the row in [first_row, last_row] whose span owns `visual_column`.
  //
  // Rows of one line are contiguous and their `visual_end` is non-decreasing, so
  // this is a binary search, not the linear walk it replaced: a megabyte line at
  // wrap 100 is ten thousand rows, and the caret's row is re-resolved several
  // times per keystroke and once per frame.
  //
  // `prefer_previous_row` picks the answer at a wrap boundary (a column that is
  // both one past row R's end and row R+1's start): false hands back R+1, true
  // hands back R. See WrapRowAffinity.
  std::size_t WrappedRowForVisualColumn(std::size_t first_row,
                                        std::size_t last_row,
                                        std::size_t visual_column,
                                        bool prefer_previous_row) const;
  bool wrapped_row_layouts_trivial() const { return wrapped_row_layouts_trivial_; }

  // ---- max visual columns cache -----------------------------------------
  // What this cache knows about `line_index`, or an unknown value when the width
  // table does not currently describe the document. Callers that only need to
  // turn a byte column into a visual one use this to answer in O(1) on a plain
  // ASCII line instead of walking the line to the column.
  LineLayoutFacts LineFactsIfCurrent(std::size_t lines_size,
                                     std::size_t line_index,
                                     std::size_t tab_size,
                                     std::uint64_t content_revision) const {
    if (!LineWidthsAreCurrent(lines_size, tab_size, content_revision) ||
        line_index >= cached_visual_line_columns_.size()) {
      return LineLayoutFacts{};
    }
    return cached_visual_line_columns_[line_index].facts();
  }

  std::size_t MaxVisualColumns(LineSpan lines,
                               std::size_t tab_size,
                               std::uint64_t content_revision) const;
  // Maintains the per-line visual-column cache after an in-place line edit.
  // Falls back to a full invalidation if the cache state predates the edit
  // or the tab_size changed.
  // `inserted_count` is a count; the widths are measured off `lines` (see
  // UpdateWrappedRowsAfterEdit) unless `splice` describes the edit precisely
  // enough to derive the new width without reading the line at all.
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        std::size_t inserted_count,
                                        LineSpan lines,
                                        std::size_t tab_size,
                                        std::uint64_t content_revision,
                                        InlineLineSplice splice = {});

  // ---- invalidation ------------------------------------------------------
  // Wipes every cache + every cache key. Equivalent to the previous
  // TextViewport::InvalidateVisualColumnCache (which despite its name also
  // wiped the wrapped-row table).
  void InvalidateAll();
  // Drops just the visible-line LRU + max-columns cache (used by tab-size
  // changes, where the wrapped-row table is separately re-keyed via
  // layout_shape_revision and rebuilt lazily on the next access).
  void ClearVisibleLineAndMaxColumns();
  // Drops just the wrapped-row table and every key it is validated against.
  //
  // This is the half a viewport copy/move has to give up: the new viewport comes
  // out with `folding_model_ == nullptr`, so a table built against the source's
  // fold model describes a fold state the copy does not have — and the cached
  // `const FoldingModel*` could later be compared against an unrelated model that
  // reused the address. The other half (per-line widths + the visible-line LRU)
  // depends on the document's bytes and the tab size, both of which the copy
  // shares, so it survives. See TextViewport's copy constructor
  // (TD-2026-08-06-138) and SetFoldingModel, which makes the same distinction.
  void DropWrappedRowLayouts();
  // Per-line partial invalidation of the visible-line LRU: removes every
  // cache entry whose `line_index >= start_line`. Used by
  // InvalidateDerivedCaches for content-tier edits.
  void InvalidateVisibleLineCacheFrom(std::size_t start_line);

  // ---- accessors for trivial-mode-sensitive callers ---------------------
  // True when the wrapped-row table was rebuilt with row_offsets matching
  // `lines_size`. Returns false in trivial mode (no offsets table exists)
  // or when the cache predates the latest edit.
  bool has_wrapped_line_row_offsets(std::size_t lines_size) const;
  std::size_t wrapped_row_layouts_size() const { return wrapped_row_layouts_.size(); }

  // ---- stats -------------------------------------------------------------
  Stats stats() const {
    return Stats{visible_line_queries_, visible_line_hits_, visible_line_evictions_};
  }
  void ResetStats() const {
    visible_line_queries_ = 0;
    visible_line_hits_ = 0;
    visible_line_evictions_ = 0;
  }

  // Heap bytes this cache is HOLDING, by container capacity rather than size --
  // a vector that shrank still owns its buffer, and "how much does an open tab
  // cost to keep open?" is a question about what is retained, not about what is
  // currently in use.
  //
  // Approximate on purpose, and it says so in the name. `std::deque` and
  // `std::unordered_map` do not expose their allocation shape, so those two are
  // estimated from element count and a per-node overhead; everything else is
  // exact. It exists because nothing answered the question at all
  // (TD-2026-08-06-142), and an estimate that tracks the real curve is what a
  // ceiling gets chosen from. It is NOT called on any production path -- see
  // the perf scenario and WorkspaceShell::TestAccess.
  std::size_t ApproximateResidentBytes() const;

#ifndef NDEBUG
  std::size_t wrapped_row_layout_build_count_for_debug() const {
    return wrapped_row_layout_build_count_;
  }
  // Number of incremental edits that took the O(edit-size) in-place fast path
  // (no suffix rows shifted, no offset table rebuilt) versus the O(suffix)
  // splice path. Used by the large-buffer edit perf regression to prove that a
  // common single-line edit does not walk the rest of the document.
  std::size_t wrapped_row_incremental_inplace_count_for_debug() const {
    return wrapped_row_incremental_inplace_count_;
  }
  std::size_t wrapped_row_incremental_splice_count_for_debug() const {
    return wrapped_row_incremental_splice_count_;
  }
  std::size_t visual_column_incremental_inplace_count_for_debug() const {
    return visual_column_incremental_inplace_count_;
  }
  // Edits whose new line width came from the splice alone -- no read of the line
  // at all. This is what makes "a keystroke does not re-measure the line" a
  // testable claim rather than a comment (TD-2026-08-05-132, item 2).
  std::size_t visual_column_splice_derived_count_for_debug() const {
    return visual_column_splice_derived_count_;
  }
#endif

 private:
  struct VisibleLineCacheKey {
    std::size_t line_index = 0;
    std::size_t horizontal_scroll = 0;
    std::size_t visible_columns = 0;
    std::size_t tab_size = 0;

    bool operator==(const VisibleLineCacheKey& o) const noexcept {
      return line_index == o.line_index && horizontal_scroll == o.horizontal_scroll &&
             visible_columns == o.visible_columns && tab_size == o.tab_size;
    }
  };

  struct VisibleLineCacheKeyHash {
    std::size_t operator()(const VisibleLineCacheKey& k) const noexcept {
      std::size_t h = k.line_index;
      h ^= k.horizontal_scroll * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.visible_columns * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= k.tab_size * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  // One line's derived layout facts, packed into a single word.
  //
  // This table holds one entry per line for the life of a tab, so on a 50k-line
  // file it is real memory, and a separate `bool` member would double it to 16
  // bytes per line to carry one bit. A visual width is bounded by the line's byte
  // length, so the top bit is free.
  struct PackedLineWidth {
    static constexpr std::size_t kPlainAsciiBit = std::size_t{1}
                                                  << (8 * sizeof(std::size_t) - 1);
    std::size_t packed = 0;

    static PackedLineWidth From(LineLayoutFacts facts) {
      return PackedLineWidth{facts.visual_columns |
                             (facts.plain_ascii ? kPlainAsciiBit : std::size_t{0})};
    }
    std::size_t visual_columns() const { return packed & ~kPlainAsciiBit; }
    LineLayoutFacts facts() const {
      return LineLayoutFacts{
          .visual_columns = visual_columns(),
          .plain_ascii = (packed & kPlainAsciiBit) != 0,
          .known = true,
      };
    }
  };

  // True when the per-line width table describes the document `lines` currently
  // holds, at this tab size. Both the max-columns query and the visible-line
  // builder read it, and every content edit either splices it (see
  // UpdateVisualColumnCacheAfterEdit) or drops it, so freshness is exactly
  // "same tab size, same content revision, same line count".
  bool LineWidthsAreCurrent(std::size_t lines_size,
                            std::size_t tab_size,
                            std::uint64_t content_revision) const {
    return cached_max_visual_columns_tab_size_ == tab_size &&
           cached_max_visual_columns_content_revision_ == content_revision &&
           cached_visual_line_columns_.size() == lines_size;
  }

#ifndef NDEBUG
  // Cross-checks every entry of the per-line width table (and the memoized
  // widest line) against a fresh measurement, but only when the table claims to
  // be current AND MICROIDE_VERIFY_LINE_WIDTH_TABLE is set in the environment.
  // See the definition for why this exists alongside the freshness predicate.
  void VerifyLineWidthTableIfRequested(LineSpan lines,
                                       std::size_t tab_size,
                                       std::uint64_t content_revision) const;
#endif

  // Appends the wrapped rows for a single logical line to `out` (always at
  // least one row, even for an empty line). Shared by the full-table build and
  // the incremental post-edit update so both use one wrap implementation.
  // `wrap_columns` must already be clamped to >= 1.
  static void WrapSingleLine(std::string_view line_text,
                             std::size_t line_index,
                             std::size_t tab_size,
                             std::size_t wrap_columns,
                             std::vector<WrappedRow>& out);

  static constexpr std::size_t kVisibleLineCacheLimit = 256;

  // Visible-line LRU.
  //
  // The recency order is an intrusive doubly-linked list threaded through the
  // map's own nodes, not a side container of keys: an `unordered_map` node's
  // address is stable across rehash AND across extract()/insert() of the same
  // node, which is what lets the recycle path below splice a node from the head
  // to the tail without allocating, and what a `std::list<Key>` of 256 nodes
  // would have paid for on first fill.
  //
  // Recency is what makes VisibleLineLayoutRefCached's reference-stability claim
  // hold for HITS, not just for builds (TD-2026-08-12-189): a FIFO evicted the
  // oldest INSERT, so an entry a caller was still reading could be recycled
  // underneath it if it had been inserted long enough ago. With move-to-tail on
  // hit, the eviction victim is strictly older than anything touched since.
  struct VisibleLineCacheEntry {
    LayoutLine layout;
    // Self key, so eviction can extract() the head without a reverse lookup.
    VisibleLineCacheKey key;
    // Null on both ends of the list. `lru_prev` is toward the least recently
    // used end (the eviction victim), `lru_next` toward the most recent.
    VisibleLineCacheEntry* lru_prev = nullptr;
    VisibleLineCacheEntry* lru_next = nullptr;
  };

  // Move every live entry's layout into the pool, up to its cap. The caller is
  // about to drop the entries themselves.
  void RetireVisibleLineLayouts();
  void VisibleLineLruUnlink(VisibleLineCacheEntry& entry) const;
  void VisibleLineLruPushBack(VisibleLineCacheEntry& entry) const;
  void VisibleLineLruTouch(VisibleLineCacheEntry& entry) const {
    if (visible_line_lru_tail_ == &entry) {
      return;
    }
    VisibleLineLruUnlink(entry);
    VisibleLineLruPushBack(entry);
  }

  mutable std::unordered_map<VisibleLineCacheKey, VisibleLineCacheEntry, VisibleLineCacheKeyHash>
      visible_line_cache_;
  // Layout buffers retired by invalidation, held for the next miss to build into.
  //
  // The eviction path already recycles an entry rather than erasing and
  // re-emplacing, because a scroll through fresh content misses on every row and
  // each miss costs the map node plus the LayoutLine's three vectors. An EDIT
  // took the other path: InvalidateVisibleLineCacheFrom erased every entry at or
  // after it, and the same repaint rebuilt those rows from nothing -- so typing
  // paid, on every visible row below the caret, three of the four allocations the
  // eviction path exists to avoid.
  //
  // The pool holds LayoutLines rather than extracted map nodes because a node
  // handle is move-only and TextLayoutCache has to stay copyable (a split pane
  // copies its sibling's caches). That leaves the map node itself to allocate;
  // the three per-row buffers, which are the ones sized to the row, are reused.
  //
  // Bounded by the cache limit, so this never holds more than the live cache
  // could have.
  mutable std::vector<LayoutLine> visible_line_layout_pool_;
  // Least / most recently used ends of the intrusive list above.
  mutable VisibleLineCacheEntry* visible_line_lru_head_ = nullptr;
  mutable VisibleLineCacheEntry* visible_line_lru_tail_ = nullptr;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;
  mutable std::size_t visible_line_evictions_ = 0;
  // Scratch for the bounded line reads a visible-row build does (the prefix scan
  // and the window itself). A member, not a local, so the capacity survives: a
  // local would allocate once per rendered row of every line that spans pieces.
  mutable std::string line_window_scratch_;

 public:
  // First byte of `line_index` that is not plain single-cell ASCII (no tab, no
  // byte >= 0x80), searched in [0, probe) and returning `probe` when there is
  // none. Byte offset IS visual column below that point, which is what lets a
  // visible-row build start mid-line without decoding what precedes it.
  //
  // Memoized, because the answer is a property of the LINE and the question is
  // asked per ROW. Soft wrap turns one long line into thousands of rows, each
  // built with an ever-larger `probe`, so the un-memoized form re-reads the whole
  // prefix on every row of every frame -- 29 MB per iteration of
  // `editor_soft_wrap_long_line_scroll`, growing with scroll depth, on the
  // default render path. This is the same shape TD-2026-08-12-187 removed one
  // level up in the whitespace walk, which is where it was found
  // (TD-2026-08-14-220).
  std::size_t PlainAsciiPrefixEnd(LineSpan lines,
                                  std::size_t line_index,
                                  std::size_t probe,
                                  std::uint64_t content_revision) const;

 private:
  // One slot per line the memo is tracking. Rows of a line arrive consecutively,
  // so a single slot already collapses a frame's per-row scans into one; the four
  // keep a document with several very long lines on screen from re-priming the
  // slot per line per frame. Round-robin replacement — with four slots and this
  // access pattern, an LRU would cost more than it saves.
  struct PlainPrefixMemo {
    std::size_t line_index = 0;
    std::uint64_t content_revision = 0;
    // How far [0, ...) has been examined, and the first offending byte found.
    // `first_non_plain == scanned_through` means "none found so far".
    std::size_t scanned_through = 0;
    std::size_t first_non_plain = 0;
    bool valid = false;
  };
  static constexpr std::size_t kPlainPrefixMemoSlots = 4;
  mutable std::array<PlainPrefixMemo, kPlainPrefixMemoSlots> plain_prefix_memo_{};
  mutable std::size_t plain_prefix_memo_next_ = 0;

  // Wrapped-row table
  mutable std::vector<WrappedRow> wrapped_row_layouts_;
  mutable std::vector<std::size_t> wrapped_line_row_offsets_;
  mutable std::size_t wrapped_row_layouts_tab_size_ = 0;
  mutable std::size_t wrapped_row_layouts_visible_columns_ = 0;
  mutable std::uint64_t wrapped_row_layouts_layout_shape_revision_ = 0;
  mutable bool wrapped_row_layouts_soft_wrap_ = false;
  mutable const FoldingModel* wrapped_row_layouts_folding_model_ = nullptr;
  mutable std::size_t wrapped_row_layouts_fold_revision_ = 0;
  // Content-tier guard: the wrapped-row table is content-derived, so a content
  // edit that is not reflected by an incremental UpdateWrappedRowsAfterEdit must
  // force a rebuild even though layout_shape_revision is unchanged.
  mutable std::uint64_t wrapped_row_layouts_content_revision_ = 0;
  mutable bool wrapped_row_layouts_trivial_ = false;
  // Fold-but-no-soft-wrap: the table records line visibility/order only; each
  // row's visual span is synthesized from the live horizontal_scroll in
  // WrappedRowAt rather than baked at build time (so a horizontal scroll does
  // not stale the cached spans nor force an O(lines) rebuild).
  mutable bool wrapped_row_layouts_fold_no_wrap_ = false;
#ifndef NDEBUG
  mutable std::size_t wrapped_row_layout_build_count_ = 0;
  mutable std::size_t wrapped_row_incremental_inplace_count_ = 0;
  mutable std::size_t wrapped_row_incremental_splice_count_ = 0;
  mutable std::size_t visual_column_incremental_inplace_count_ = 0;
  mutable std::size_t visual_column_splice_derived_count_ = 0;
#endif

  // Visual-column width cache
  mutable std::optional<std::size_t> cached_max_visual_columns_;
  mutable std::optional<std::size_t> cached_max_visual_columns_line_index_;
  mutable std::deque<PackedLineWidth> cached_visual_line_columns_;
  // Scratch for UpdateVisualColumnCacheAfterEdit's inserted-line widths. A local
  // vector there is one heap allocation per edit -- i.e. per keystroke, on the
  // shell thread -- for what is usually a single value.
  mutable std::vector<PackedLineWidth> inserted_columns_scratch_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::uint64_t cached_max_visual_columns_content_revision_ = 0;
};

}  // namespace microide::editor
