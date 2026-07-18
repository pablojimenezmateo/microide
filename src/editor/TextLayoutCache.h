#pragma once

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
  };

  // ---- visible-line LayoutLine LRU --------------------------------------
  // Returns the LayoutLine for `line_index` from the cache, or builds it on
  // miss. Caller is responsible for caret-row decoration (the cache does not
  // know which row the cursor is on).
  LayoutLine VisibleLineLayoutCached(LineSpan lines,
                                     std::size_t line_index,
                                     std::size_t horizontal_scroll,
                                     std::size_t visible_columns,
                                     std::size_t tab_size) const;

  // Reference-returning variant: hands back the cached LayoutLine in place
  // instead of copying it out. The returned reference is stable until the next
  // call that can evict it (the per-frame working set is far below
  // kVisibleLineCacheLimit, so a renderer that consumes each row before
  // requesting the next never sees its own entry evicted mid-use). Like the
  // by-value variant, the cache does not set caret fields.
  const LayoutLine& VisibleLineLayoutRefCached(LineSpan lines,
                                               std::size_t line_index,
                                               std::size_t horizontal_scroll,
                                               std::size_t visible_columns,
                                               std::size_t tab_size) const;

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
  bool UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                  std::size_t removed_count,
                                  const std::vector<std::string>& inserted_lines,
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
  bool wrapped_row_layouts_trivial() const { return wrapped_row_layouts_trivial_; }

  // ---- max visual columns cache -----------------------------------------
  std::size_t MaxVisualColumns(LineSpan lines,
                               std::size_t tab_size,
                               std::uint64_t content_revision) const;
  // Maintains the per-line visual-column cache after an in-place line edit.
  // Falls back to a full invalidation if the cache state predates the edit
  // or the tab_size changed.
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        const std::vector<std::string>& inserted_lines,
                                        LineSpan lines,
                                        std::size_t tab_size,
                                        std::uint64_t content_revision);

  // ---- invalidation ------------------------------------------------------
  // Wipes every cache + every cache key. Equivalent to the previous
  // TextViewport::InvalidateVisualColumnCache (which despite its name also
  // wiped the wrapped-row table).
  void InvalidateAll();
  // Drops just the visible-line LRU + max-columns cache (used by tab-size
  // changes, where the wrapped-row table is separately re-keyed via
  // layout_shape_revision and rebuilt lazily on the next access).
  void ClearVisibleLineAndMaxColumns();
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
  Stats stats() const { return Stats{visible_line_queries_, visible_line_hits_}; }
  void ResetStats() const {
    visible_line_queries_ = 0;
    visible_line_hits_ = 0;
  }

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

  // Visible-line LRU
  mutable std::unordered_map<VisibleLineCacheKey, LayoutLine, VisibleLineCacheKeyHash>
      visible_line_cache_;
  mutable std::deque<VisibleLineCacheKey> visible_line_cache_order_;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;

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
#endif

  // Visual-column width cache
  mutable std::optional<std::size_t> cached_max_visual_columns_;
  mutable std::optional<std::size_t> cached_max_visual_columns_line_index_;
  mutable std::deque<std::size_t> cached_visual_line_columns_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::uint64_t cached_max_visual_columns_content_revision_ = 0;
};

}  // namespace microide::editor
