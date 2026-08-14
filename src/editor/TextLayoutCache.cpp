#include "editor/TextLayoutCache.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>

#include "editor/FoldingModel.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

// Bytes read at a time when scanning a line's plain-ASCII prefix.
constexpr std::size_t kPrefixScanChunkBytes = 4096;

// Largest window the bounded read is worth taking; past it, fall back to the
// whole-line build. See the use site for why a window can get that big at all.
constexpr std::size_t kMaxLineWindowBytes = 64u * 1024u;

// Offset of the first tab-or-multibyte byte of line `index` within [0, limit),
// or `limit` when that prefix is all plain single-cell ASCII.
//
// Read in chunks rather than from a whole-line view: on a piece-tree source
// `lines[index]` materializes a copy of any line that spans pieces -- every line
// an in-line edit has touched -- so a file with no line breaks in it paid a
// multi-megabyte copy per rendered row per frame (TD-2026-08-05-133). The scan
// itself is already bounded by the horizontal scroll offset, which is what makes
// the chunked form finite.
std::size_t FirstNonPlainAsciiByteInRange(LineSpan lines, std::size_t index, std::size_t from,
                                          std::size_t limit, std::string& scratch) {
  std::size_t offset = from;
  while (offset < limit) {
    const std::string_view chunk =
        lines.LineWindow(index, offset, std::min(kPrefixScanChunkBytes, limit - offset), scratch);
    if (chunk.empty()) {
      break;
    }
    const std::size_t hit = util::FirstNonAsciiOrByte(chunk, '\t');
    if (hit < chunk.size()) {
      return offset + hit;
    }
    offset += chunk.size();
  }
  return limit;
}

}  // namespace

std::size_t TextLayoutCache::PlainAsciiPrefixEnd(LineSpan lines,
                                                 std::size_t line_index,
                                                 std::size_t probe,
                                                 std::uint64_t content_revision) const {
  if (probe == 0) {
    return 0;
  }
  // The line's own length joins the key. Revision alone identifies a line within
  // one document, and a viewport handed a different document can see a revision
  // repeat; a line of a different length is then a miss rather than a wrong
  // answer. Costs one O(1) lookup and does not read the line's bytes.
  const std::size_t line_length = lines.LineLength(line_index);
  for (PlainPrefixMemo& memo : plain_prefix_memo_) {
    if (!memo.valid || memo.line_index != line_index || memo.line_length != line_length ||
        memo.content_revision != content_revision) {
      continue;
    }
    if (memo.first_non_plain < memo.scanned_through) {
      // Already found the line's first offending byte; every larger probe has the
      // same answer, clipped.
      return std::min(memo.first_non_plain, probe);
    }
    if (probe <= memo.scanned_through) {
      return probe;  // known clean this far
    }
    // Extend the scan, reading only the bytes nobody has read yet.
    const std::size_t hit = FirstNonPlainAsciiByteInRange(lines, line_index, memo.scanned_through,
                                                          probe, line_window_scratch_);
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                                std::min(hit, probe) - memo.scanned_through);
    memo.scanned_through = probe;
    memo.first_non_plain = hit;
    return std::min(hit, probe);
  }

  const std::size_t hit =
      FirstNonPlainAsciiByteInRange(lines, line_index, 0, probe, line_window_scratch_);
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                              std::min(hit, probe));
  PlainPrefixMemo& slot = plain_prefix_memo_[plain_prefix_memo_next_];
  plain_prefix_memo_next_ = (plain_prefix_memo_next_ + 1) % kPlainPrefixMemoSlots;
  slot = PlainPrefixMemo{line_index, line_length, content_revision, probe, hit, true};
  return hit;
}

const LayoutLine& TextLayoutCache::VisibleLineLayoutRefCached(LineSpan lines,
                                                              std::size_t line_index,
                                                              std::size_t horizontal_scroll,
                                                              std::size_t visible_columns,
                                                              std::size_t tab_size,
                                                              std::uint64_t content_revision,
                                                              LineLayoutFacts facts_hint) const {
  ++visible_line_queries_;
  const VisibleLineCacheKey cache_key{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll,
      .visible_columns = visible_columns,
      .tab_size = tab_size,
  };
  if (const auto it = visible_line_cache_.find(cache_key); it != visible_line_cache_.end()) {
    ++visible_line_hits_;
    // A hit is what makes this an LRU rather than a FIFO: without it, an entry
    // read on every frame still ages out by insertion order and can be recycled
    // while a caller holds the reference this returns (TD-2026-08-12-189).
    VisibleLineLruTouch(it->second);
    return it->second.layout;
  }
  // Hand the builder what this cache already knows about the line, so a miss does
  // not re-walk it for its visual width nor step code points to reach the first
  // visible cell. Both are O(line), and on a line with no newlines in it that is
  // the dominant cost of rendering a row (TD-2026-08-05-132, item 1).
  LineLayoutFacts facts =
      LineFactsIfCurrent(lines.size(), line_index, tab_size, content_revision);
  [[maybe_unused]] const bool facts_from_width_table = facts.known;
  if (!facts.known) {
    facts = facts_hint;
  }
#ifndef NDEBUG
  // The table is only as good as the invariant that every content edit either
  // splices it or drops it. Cross-check it here, where a stale entry would
  // silently mislay a row's glyphs rather than merely mis-size the scrollbar.
  // Bounded by line length so the check costs nothing on the pathological lines
  // this whole path exists for. A caller-supplied hint is checked the same way --
  // except for `plain_ascii`, which a hint is allowed to understate (the
  // wrapped-row table knows widths, not encodings) but never to overstate.
  if (facts.known && lines.LineLength(line_index) <= 4096) {
    const LineLayoutFacts measured = TextLayout::MeasureLineFacts(lines[line_index], tab_size);
    assert(measured.visual_columns == facts.visual_columns &&
           "per-line width table went stale against the buffer");
    assert((facts_from_width_table ? measured.plain_ascii == facts.plain_ascii
                                   : (!facts.plain_ascii || measured.plain_ascii)) &&
           "line layout facts claim plain ASCII for a line that is not");
  }
#endif
  // Read only the bytes the build can visit, when the width table can tell us
  // where the visible walk starts. Without `facts` the line's full visual width
  // has to be walked, which needs all of it -- the whole-line form below.
  //
  // The window is bounded by the row, not by the line: `lines[line_index]` asks
  // for the whole line, and on a piece-tree source that materializes any line that
  // spans pieces -- every line an in-line edit has touched. On a file with no line
  // breaks in it that was a multi-megabyte copy per frame, and it was the last
  // caller keeping that copy alive (TD-2026-08-05-133).
  TextLayout::VisibleLineWindow window;
  bool windowed = false;
  if (facts.known) {
    const std::size_t line_length = lines.LineLength(line_index);
    const std::size_t probe = std::min(horizontal_scroll, line_length);
    // Memoized: the scan's answer is a property of the line, and soft wrap asks it
    // once per row of the same line with an ever-larger probe. The counter now
    // reports bytes actually READ, so it goes to (almost) zero rather than merely
    // being amortized (TD-2026-08-14-220).
    const std::size_t plain_prefix_end =
        facts.plain_ascii ? line_length
                          : PlainAsciiPrefixEnd(lines, line_index, probe, content_revision);
    const std::size_t start_byte = TextLayout::ComputeVisibleLineWindowStart(
        horizontal_scroll, line_length, plain_prefix_end);
    const std::size_t want =
        TextLayout::VisibleLineWindowBytes(start_byte, horizontal_scroll, visible_columns);
    // A window only pays when it is small. On a line whose first tab or multibyte
    // byte comes before the scroll offset, the walk has to start there and cover
    // every column up to the visible ones, so the window can be the rest of the
    // line -- and `line_window_scratch_` would then hold a copy of it for the life
    // of the cache, where the whole-line form's copy lives in the piece tree's
    // per-revision cache and is dropped by the next edit. A rendered row asks for
    // one or two kilobytes, so this bound is far above the real case and only the
    // degenerate one falls through it.
    if (want <= kMaxLineWindowBytes) {
      window = TextLayout::VisibleLineWindow{
          .bytes = lines.LineWindow(line_index, start_byte, want, line_window_scratch_),
          .start_byte = start_byte,
          .line_length = line_length,
      };
      windowed = true;
    }
  }
  const auto build_into = [&](LayoutLine& out) {
    if (windowed) {
      TextLayout::BuildVisibleLineWindowInto(window, horizontal_scroll, visible_columns, tab_size,
                                             out, facts);
    } else {
      TextLayout::BuildVisibleLineInto(lines[line_index], horizontal_scroll, visible_columns,
                                       tab_size, out, facts);
    }
  };
  if (visible_line_cache_.size() >= kVisibleLineCacheLimit) {
    // This is the only point that can invalidate a reference previously handed
    // out by this function. Counted so the "a frame never evicts what it is
    // still reading" invariant is measurable rather than merely asserted.
    ++visible_line_evictions_;
    // Counted here and NOT on the query/hit path. Those two already exist as plain
    // non-atomic members behind stats(), and AddPerformanceCounter is a locked
    // read-modify-write on a shared cache line — not something to put on a
    // per-visible-row render path. Evictions and recycles are rare, and they are
    // what this cache's behaviour actually turns on.
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutEvictions);
    // Recycle the evicted entry rather than destroying it and building a fresh
    // one. Scrolling through fresh content misses on EVERY row, so this path is
    // the steady state there, and erase()+emplace() paid four allocations per row
    // — the map node plus the LayoutLine's text, source_columns and text_offsets
    // (~3.4 KB for a 200-column window) — only to free the identical set when the
    // entry reached the front of the LRU a few hundred rows later. `extract`
    // hands back the node with its buffers intact; rewriting the key and
    // reinserting reuses all four.
    //
    // The victim is the LRU head, so it is older than every entry any caller has
    // read since — including the ones this frame merely HIT.
    assert(visible_line_lru_head_ != nullptr);
    VisibleLineCacheEntry& victim = *visible_line_lru_head_;
    VisibleLineLruUnlink(victim);
    auto node = visible_line_cache_.extract(victim.key);
    if (!node.empty()) {
      build_into(node.mapped().layout);
      node.mapped().key = cache_key;
      node.key() = cache_key;
      util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);
      const auto result = visible_line_cache_.insert(std::move(node));
      VisibleLineLruPushBack(result.position->second);
      return result.position->second.layout;
    }
  }
  const auto [it, _] = visible_line_cache_.emplace(cache_key, VisibleLineCacheEntry{});
  it->second.key = cache_key;
  // A layout retired by the last invalidation carries the same three buffers this
  // row would otherwise allocate from scratch; build_into overwrites the contents
  // and keeps the capacity.
  if (!visible_line_layout_pool_.empty()) {
    it->second.layout = std::move(visible_line_layout_pool_.back());
    visible_line_layout_pool_.pop_back();
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);
  }
  build_into(it->second.layout);
  VisibleLineLruPushBack(it->second);
  return it->second.layout;
}

void TextLayoutCache::VisibleLineLruUnlink(VisibleLineCacheEntry& entry) const {
  if (entry.lru_prev != nullptr) {
    entry.lru_prev->lru_next = entry.lru_next;
  } else if (visible_line_lru_head_ == &entry) {
    visible_line_lru_head_ = entry.lru_next;
  }
  if (entry.lru_next != nullptr) {
    entry.lru_next->lru_prev = entry.lru_prev;
  } else if (visible_line_lru_tail_ == &entry) {
    visible_line_lru_tail_ = entry.lru_prev;
  }
  entry.lru_prev = nullptr;
  entry.lru_next = nullptr;
}

void TextLayoutCache::VisibleLineLruPushBack(VisibleLineCacheEntry& entry) const {
  entry.lru_prev = visible_line_lru_tail_;
  entry.lru_next = nullptr;
  if (visible_line_lru_tail_ != nullptr) {
    visible_line_lru_tail_->lru_next = &entry;
  } else {
    visible_line_lru_head_ = &entry;
  }
  visible_line_lru_tail_ = &entry;
}


void TextLayoutCache::WrapSingleLine(std::string_view line_text,
                                     std::size_t line_index,
                                     std::size_t tab_size,
                                     std::size_t wrap_columns,
                                     std::vector<WrappedRow>& out) {
  // Thin adapter over the one wrap implementation (TextLayout::WrapLineSegments),
  // which the compare/merge diff surfaces share -- so a break decision cannot
  // differ between a file in the editor and the same file in a diff pane.
  TextLayout::WrapLineSegments(
      line_text, tab_size, wrap_columns,
      [&out, line_index](std::size_t visual_start, std::size_t visual_end, std::size_t indent) {
        out.push_back(WrappedRow{line_index, visual_start, visual_end, indent});
      });
}

void TextLayoutCache::EnsureWrappedRowLayouts(LineSpan lines,
                                              std::size_t tab_size,
                                              std::size_t visible_columns,
                                              bool soft_wrap,
                                              const FoldingModel* folding_model,
                                              std::uint64_t layout_shape_revision,
                                              std::uint64_t content_revision) const {
  // O(1) collapsed-fold probe (the previous std::vector<bool> linear scan was
  // paid on every edit before the maintained counter landed).
  const bool has_any_collapsed_fold =
      folding_model != nullptr && folding_model->has_any_collapsed_fold();
  const bool trivial_now = !soft_wrap && !has_any_collapsed_fold;

  // Trivial-layout cache: in trivial mode the readers (WrappedRowAt,
  // WrappedRowCount, WrappedLineRowOffset) synthesize from the **current**
  // horizontal_scroll / visible_columns / tab_size — they do not read the
  // cached `wrapped_row_layouts_` vectors. So once trivial, the cache stays
  // valid until trivial_now flips.
  if (wrapped_row_layouts_trivial_ && trivial_now) {
    return;
  }

  if (!trivial_now && wrapped_row_layouts_layout_shape_revision_ == layout_shape_revision &&
      wrapped_row_layouts_content_revision_ == content_revision &&
      wrapped_row_layouts_tab_size_ == tab_size &&
      wrapped_row_layouts_visible_columns_ == visible_columns &&
      wrapped_row_layouts_soft_wrap_ == soft_wrap &&
      wrapped_row_layouts_folding_model_ == folding_model &&
      // `layout_revision`, NOT `revision`: which rows exist depends only on which
      // lines are HIDDEN, i.e. on the collapsed set. The fold model also bumps
      // its general revision every time a scroll resolves a different window,
      // and keying on that would rebuild the whole document's row table on every
      // scrolled frame with any fold collapsed.
      wrapped_row_layouts_fold_revision_ ==
          (folding_model != nullptr ? folding_model->layout_revision() : 0)) {
    return;
  }

  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);

  if (!soft_wrap && !has_any_collapsed_fold) {
    wrapped_row_layouts_trivial_ = true;
    wrapped_row_layouts_fold_no_wrap_ = false;
    util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits, 0);
    wrapped_row_layouts_tab_size_ = tab_size;
    wrapped_row_layouts_visible_columns_ = visible_columns;
    wrapped_row_layouts_layout_shape_revision_ = layout_shape_revision;
    wrapped_row_layouts_content_revision_ = content_revision;
    wrapped_row_layouts_soft_wrap_ = soft_wrap;
    wrapped_row_layouts_folding_model_ = folding_model;
    wrapped_row_layouts_fold_revision_ =
        folding_model != nullptr ? folding_model->layout_revision() : 0;
#ifndef NDEBUG
    ++wrapped_row_layout_build_count_;
#endif
    return;
  }

  wrapped_row_layouts_trivial_ = false;
  wrapped_row_layouts_fold_no_wrap_ = !soft_wrap;
  wrapped_row_layouts_.reserve(lines.size());
  wrapped_line_row_offsets_.reserve(lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits,
                              lines.size());
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns);
  std::size_t last_visible_row = 0;
  if (!soft_wrap) {
    // Non-soft-wrap with collapsed folds: the table records line
    // visibility/order only. The visual span is synthesized in WrappedRowAt
    // from the live horizontal_scroll, so nothing scroll-dependent is baked.
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      wrapped_row_layouts_.push_back(WrappedRow{line_index, 0, 0, 0});
    }
  } else {
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      WrapSingleLine(lines[line_index], line_index, tab_size, wrap_columns,
                     wrapped_row_layouts_);
    }
  }
  if (wrapped_row_layouts_.empty()) {
    wrapped_row_layouts_.push_back(WrappedRow{0, 0, 0});
    wrapped_line_row_offsets_.assign(lines.size(), 0);
  }
  wrapped_row_layouts_tab_size_ = tab_size;
  wrapped_row_layouts_visible_columns_ = visible_columns;
  wrapped_row_layouts_layout_shape_revision_ = layout_shape_revision;
  wrapped_row_layouts_content_revision_ = content_revision;
  wrapped_row_layouts_soft_wrap_ = soft_wrap;
  wrapped_row_layouts_folding_model_ = folding_model;
  wrapped_row_layouts_fold_revision_ =
      folding_model != nullptr ? folding_model->layout_revision() : 0;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
}

bool TextLayoutCache::UpdateWrappedRowsAfterEdit(
    std::size_t start_line, std::size_t removed_count, std::size_t inserted_count,
    LineSpan lines,
    std::size_t tab_size, std::size_t visible_columns, bool soft_wrap,
    const FoldingModel* folding_model, std::uint64_t layout_shape_revision,
    std::uint64_t content_revision) const {
  // Only the pure soft-wrap path (no collapsed folds) is spliced incrementally.
  // Every other mode bails so the content_revision guard forces a full rebuild.
  const bool has_any_collapsed_fold =
      folding_model != nullptr && folding_model->has_any_collapsed_fold();
  if (!soft_wrap || has_any_collapsed_fold || wrapped_row_layouts_trivial_ ||
      wrapped_row_layouts_fold_no_wrap_ || !wrapped_row_layouts_soft_wrap_ ||
      wrapped_row_layouts_tab_size_ != tab_size ||
      wrapped_row_layouts_visible_columns_ != visible_columns ||
      wrapped_row_layouts_layout_shape_revision_ != layout_shape_revision ||
      wrapped_row_layouts_folding_model_ != folding_model ||
      wrapped_row_layouts_fold_revision_ !=
          (folding_model != nullptr ? folding_model->layout_revision() : 0)) {
    return false;
  }

  // The table must be in sync with the *pre-edit* buffer: one offset entry per
  // pre-edit logical line, contiguous rows per line (pure soft-wrap has no
  // hidden lines). old_line_count is derived from the new buffer + edit deltas.
  if (lines.size() + removed_count < inserted_count) {
    return false;
  }
  const std::size_t old_line_count = lines.size() + removed_count - inserted_count;
  if (wrapped_line_row_offsets_.size() != old_line_count || old_line_count == 0 ||
      start_line + removed_count > old_line_count) {
    return false;
  }

  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns);
  const std::size_t total_rows = wrapped_row_layouts_.size();
  // A pure append at end-of-document has start_line == old_line_count, which is
  // one past the last valid offset entry. Treat it as "starts after the last
  // row" (mirrors the removed_rows_end guard below) instead of reading OOB.
  const std::size_t first_row =
      start_line < old_line_count ? wrapped_line_row_offsets_[start_line] : total_rows;
  const std::size_t removed_end_line = start_line + removed_count;
  const std::size_t removed_rows_end =
      removed_end_line < old_line_count ? wrapped_line_row_offsets_[removed_end_line] : total_rows;
  if (first_row > removed_rows_end || removed_rows_end > total_rows) {
    return false;
  }

  // Re-wrap only the inserted lines (read from the live buffer).
  std::vector<WrappedRow> new_rows;
  new_rows.reserve(inserted_count);
  std::vector<std::size_t> inserted_offsets;  // row offset (relative to first_row) per inserted line
  inserted_offsets.reserve(inserted_count);
  for (std::size_t i = 0; i < inserted_count; ++i) {
    inserted_offsets.push_back(new_rows.size());
    WrapSingleLine(lines[start_line + i], start_line + i, tab_size, wrap_columns, new_rows);
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(inserted_count) - static_cast<std::ptrdiff_t>(removed_count);
  const std::size_t removed_rows = removed_rows_end - first_row;

  // In-place fast path: the edit changed neither the logical-line count
  // (line_delta == 0) nor the wrapped-row count of the affected region
  // (removed_rows == new_rows.size()). This is the dominant keystroke — typing
  // or deleting inside a line that still wraps to the same rows. The prefix
  // rows, every suffix row's line_index, and the prefix/suffix of the
  // line->row offset table are all unchanged, so overwrite only the affected
  // rows (and the affected lines' own offsets, which may redistribute among
  // themselves) and touch nothing in the O(document) suffix.
  if (line_delta == 0 && new_rows.size() == removed_rows) {
    std::copy(new_rows.begin(), new_rows.end(),
              wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(first_row));
    for (std::size_t i = 0; i < inserted_count; ++i) {
      wrapped_line_row_offsets_[start_line + i] = first_row + inserted_offsets[i];
    }
    wrapped_row_layouts_content_revision_ = content_revision;
#ifndef NDEBUG
    ++wrapped_row_incremental_inplace_count_;
#endif
    return true;
  }

  // Splice rows in place: shift the trailing rows' line_index by line_delta,
  // then replace [first_row, removed_rows_end) with the freshly wrapped rows.
  // The line_index shift is a pure no-op when line_delta == 0 (an in-line edit
  // that only changed the wrap-row count), so skip that O(suffix) scalar pass.
  if (line_delta != 0) {
    for (std::size_t r = removed_rows_end; r < total_rows; ++r) {
      wrapped_row_layouts_[r].line_index = static_cast<std::size_t>(
          static_cast<std::ptrdiff_t>(wrapped_row_layouts_[r].line_index) + line_delta);
    }
  }
#ifndef NDEBUG
  ++wrapped_row_incremental_splice_count_;
#endif
  wrapped_row_layouts_.erase(
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(first_row),
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(removed_rows_end));
  wrapped_row_layouts_.insert(
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(first_row),
      new_rows.begin(), new_rows.end());

  // Rebuild the line->row offset table for the new line count. Prefix offsets
  // are unchanged; inserted lines use first_row + their relative offset; the
  // suffix shifts by the row-count delta.
  const std::ptrdiff_t row_delta = static_cast<std::ptrdiff_t>(new_rows.size()) -
                                   static_cast<std::ptrdiff_t>(removed_rows_end - first_row);
  const std::size_t new_line_count = lines.size();  // == old_line_count + line_delta
  std::vector<std::size_t> new_offsets;
  new_offsets.reserve(new_line_count);
  for (std::size_t l = 0; l < start_line; ++l) {
    new_offsets.push_back(wrapped_line_row_offsets_[l]);
  }
  for (std::size_t i = 0; i < inserted_count; ++i) {
    new_offsets.push_back(first_row + inserted_offsets[i]);
  }
  for (std::size_t l = removed_end_line; l < old_line_count; ++l) {
    new_offsets.push_back(static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(wrapped_line_row_offsets_[l]) + row_delta));
  }
  if (new_offsets.size() != new_line_count || wrapped_row_layouts_.empty()) {
    // Shape mismatch (should not happen); fall back to a full rebuild.
    return false;
  }
  wrapped_line_row_offsets_ = std::move(new_offsets);
  wrapped_row_layouts_content_revision_ = content_revision;
  return true;
}

TextLayoutCache::WrappedRow TextLayoutCache::WrappedRowAt(std::size_t visual_row_index,
                                                          std::size_t horizontal_scroll,
                                                          std::size_t visible_columns) const {
  if (wrapped_row_layouts_trivial_) {
    return WrappedRow{visual_row_index, horizontal_scroll, horizontal_scroll + visible_columns, 0};
  }
  if (wrapped_row_layouts_.empty()) {
    return WrappedRow{};
  }
  const std::size_t clamped =
      std::min<std::size_t>(visual_row_index, wrapped_row_layouts_.size() - 1);
  if (wrapped_row_layouts_fold_no_wrap_) {
    // Fold-but-no-wrap: only line_index is recorded; the visible span tracks
    // the live horizontal scroll window.
    return WrappedRow{wrapped_row_layouts_[clamped].line_index, horizontal_scroll,
                      horizontal_scroll + visible_columns, 0};
  }
  return wrapped_row_layouts_[clamped];
}

std::pair<std::size_t, std::size_t> TextLayoutCache::WrappedRowRangeForLine(
    std::size_t line_index, std::size_t lines_size) const {
  (void)lines_size;
  const std::size_t first = WrappedLineRowOffset(line_index);
  if (wrapped_row_layouts_.empty()) {
    return {first, first};
  }
  const std::size_t row_count = wrapped_row_layouts_.size();
  const std::size_t clamped_first = std::min(first, row_count - 1);
  // A line's wrapped rows are contiguous and all tagged with its line_index; the
  // next row belongs to a different (visible) line. Scan this line's own rows to
  // find its last row directly. Inferring `last` from the next logical line's
  // offset was wrong when that line is hidden by a collapsed fold: hidden lines
  // contribute no rows and reuse the opener's offset, so a soft-wrapped fold
  // opener (rows [R, R+k-1]) collapsed to {R, R}, sticking vertical motion on it.
  const std::size_t owner_line = wrapped_row_layouts_[clamped_first].line_index;
  // `line_index` is non-decreasing across the table, so the first row past this
  // line is a binary search rather than a walk. The walk was O(rows in the line)
  // -- on a megabyte line at a narrow wrap width that is tens of thousands of
  // steps, paid every time the caret's row is resolved.
  const auto* const begin = wrapped_row_layouts_.data();
  const auto* const past = std::partition_point(
      begin + static_cast<std::ptrdiff_t>(clamped_first), begin + static_cast<std::ptrdiff_t>(row_count),
      [owner_line](const WrappedRow& row) { return row.line_index <= owner_line; });
  const std::size_t last = static_cast<std::size_t>(past - begin);
  return {clamped_first, last > clamped_first ? last - 1 : clamped_first};
}

std::size_t TextLayoutCache::WrappedRowForVisualColumn(std::size_t first_row,
                                                       std::size_t last_row,
                                                       std::size_t visual_column,
                                                       bool prefer_previous_row) const {
  if (wrapped_row_layouts_.empty() || last_row <= first_row) {
    return first_row;
  }
  const std::size_t row_count = wrapped_row_layouts_.size();
  const std::size_t clamped_last = std::min(last_row, row_count - 1);
  if (first_row >= clamped_last) {
    return std::min(first_row, clamped_last);
  }
  // Row spans partition the line's visual columns, so the owning row is the
  // first one whose end is past the caret -- or, with previous-row affinity, the
  // first one whose end reaches it. Only rows [first_row, clamped_last) are
  // searched: the line's last row owns everything at or past its end (the
  // end-of-line caret included), so it is the fallthrough answer either way.
  const auto* const begin = wrapped_row_layouts_.data();
  const auto* const hit = std::partition_point(
      begin + static_cast<std::ptrdiff_t>(first_row),
      begin + static_cast<std::ptrdiff_t>(clamped_last),
      [visual_column, prefer_previous_row](const WrappedRow& row) {
        return prefer_previous_row ? row.visual_end < visual_column
                                   : row.visual_end <= visual_column;
      });
  return static_cast<std::size_t>(hit - begin);
}

std::size_t TextLayoutCache::WrappedRowCount(std::size_t lines_size) const {
  if (wrapped_row_layouts_trivial_) {
    return lines_size;
  }
  return wrapped_row_layouts_.size();
}

std::size_t TextLayoutCache::WrappedLineRowOffset(std::size_t line_index) const {
  if (wrapped_row_layouts_trivial_) {
    return line_index;
  }
  if (wrapped_line_row_offsets_.empty() || line_index >= wrapped_line_row_offsets_.size()) {
    return 0;
  }
  return wrapped_line_row_offsets_[line_index];
}

#ifndef NDEBUG
void TextLayoutCache::VerifyLineWidthTableIfRequested(LineSpan lines,
                                                      std::size_t tab_size,
                                                      std::uint64_t content_revision) const {
  // Opt-in, debug-only, O(document): the second half of TD-2026-08-06-143. Making
  // the freshness predicate whole (below) proves the table is never READ at a
  // revision it was not built for; it cannot prove the entries themselves are
  // right, because the incremental splice path derives widths rather than
  // measuring them. This is what turns "every edit path maintains the table
  // correctly" from a comment into something a soak run can falsify.
  //
  // Off by default and never compiled into a release build, so it costs nothing
  // unless MICROIDE_VERIFY_LINE_WIDTH_TABLE is set.
  static const bool enabled = [] {
    const char* value = std::getenv("MICROIDE_VERIFY_LINE_WIDTH_TABLE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  if (!enabled || !LineWidthsAreCurrent(lines.size(), tab_size, content_revision)) {
    return;
  }
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const LineLayoutFacts fresh = TextLayout::MeasureLineFacts(lines[index], tab_size);
    const LineLayoutFacts cached = cached_visual_line_columns_[index].facts();
    assert(cached.visual_columns == fresh.visual_columns &&
           "width table entry disagrees with the line it claims to describe");
    assert(cached.plain_ascii == fresh.plain_ascii &&
           "width table plain-ASCII bit disagrees with the line it claims to describe");
  }
  if (cached_max_visual_columns_.has_value()) {
    std::size_t widest = 0;
    for (std::size_t index = 0; index < cached_visual_line_columns_.size(); ++index) {
      widest = std::max(widest, cached_visual_line_columns_[index].visual_columns());
    }
    assert(*cached_max_visual_columns_ == widest &&
           "memoized widest line disagrees with the width table it was derived from");
  }
}
#endif

std::size_t TextLayoutCache::MaxVisualColumns(LineSpan lines,
                                              std::size_t tab_size,
                                              std::uint64_t content_revision) const {
#ifndef NDEBUG
  VerifyLineWidthTableIfRequested(lines, tab_size, content_revision);
#endif
  if (cached_max_visual_columns_.has_value() && cached_max_visual_columns_tab_size_ == tab_size &&
      cached_max_visual_columns_content_revision_ == content_revision) {
    return *cached_max_visual_columns_;
  }

  // The width table is only reusable when it describes THIS document at THIS tab
  // size -- which is exactly `LineWidthsAreCurrent`, the predicate every reader of
  // the table already goes through. MaxVisualColumns used to check two of its three
  // terms and skip the content revision, then stamp the revision it had not
  // verified, so a content edit that kept the line count and did not splice the
  // table left it stale AND marked current: the max came from pre-edit widths, and
  // every later LineFactsIfCurrent caller (caret column conversion, rendered rows)
  // believed a width for text that was no longer there (TD-2026-08-06-143).
  //
  // Sharing one predicate costs nothing on any live path: every edit path either
  // splices the table through UpdateVisualColumnCacheAfterEdit (which stamps the
  // post-edit revision, because the invalidation that bumps it runs first) or drops
  // the table whole. A stale-revision rebuild is therefore both correct and, today,
  // never taken -- and its counter is what says so out loud instead of by comment.
  if (!LineWidthsAreCurrent(lines.size(), tab_size, content_revision)) {
    util::PerformanceTrace::Scope s("TextLayoutCache::BuildVisualLineColumns");
    // One reason per build, so the four reasons sum to the build count. Cold is
    // checked first because an empty table is simultaneously a tab-size and a
    // line-count mismatch, and attributing it to either would hide the real
    // repeat-build shapes (TD-2026-08-06-138).
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds);
    if (cached_visual_line_columns_.empty()) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildCold);
    } else if (cached_max_visual_columns_tab_size_ != tab_size) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildTabSize);
    } else if (cached_visual_line_columns_.size() != lines.size()) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildLineCount);
    } else {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildStaleRevision);
    }
    cached_visual_line_columns_.assign(lines.size(), PackedLineWidth{});
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthFullMeasures, lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
      // One LineSpan read per line: `operator[]` goes through an indirect call
      // into the piece tree, and this loop asked for the same line twice (once
      // for the text, once for its length) on every line of the document.
      cached_visual_line_columns_[index] =
          PackedLineWidth::From(TextLayout::MeasureLineFacts(lines[index], tab_size));
    }
  }

  util::PerformanceTrace::Scope sm("TextLayoutCache::ScanVisualLineColumnsMax");
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxScans);
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxScanLines,
                              cached_visual_line_columns_.size());
  std::size_t max_columns = 0;
  std::size_t max_line = 0;
  for (std::size_t index = 0; index < cached_visual_line_columns_.size(); ++index) {
    if (cached_visual_line_columns_[index].visual_columns() >= max_columns) {
      max_columns = cached_visual_line_columns_[index].visual_columns();
      max_line = index;
    }
  }
  cached_max_visual_columns_ = max_columns;
  cached_max_visual_columns_line_index_ = max_line;
  cached_max_visual_columns_tab_size_ = tab_size;
  cached_max_visual_columns_content_revision_ = content_revision;
  return *cached_max_visual_columns_;
}

void TextLayoutCache::UpdateVisualColumnCacheAfterEdit(
    std::size_t start_line, std::size_t removed_count, std::size_t inserted_count,
    LineSpan lines,
    std::size_t tab_size, std::uint64_t content_revision, InlineLineSplice splice) {
  if (cached_max_visual_columns_tab_size_ != tab_size ||
      lines.size() + removed_count < inserted_count ||
      cached_visual_line_columns_.size() != lines.size() - inserted_count + removed_count) {
    // Reset only the visual-column + visible-line caches here. The wrapped-row
    // table is content-guarded separately (content_revision) and updated
    // incrementally by UpdateWrappedRowsAfterEdit, so it must not be wiped on
    // this hot path — doing so forced a full O(document) re-wrap per keystroke
    // under soft wrap.
    ClearVisibleLineAndMaxColumns();
    return;
  }

  // A single line replaced in place by a byte splice keeps its width derivable
  // without reading it: the line was plain ASCII (no tab, no byte >= 0x80), so it
  // stays plain exactly when the spliced-in text is, and a plain line's visual
  // width is its byte count. That turns a megabyte scan per keystroke on a line
  // with no newlines in it into a scan of the one or two bytes typed
  // (TD-2026-08-05-132, item 2).
  const bool inline_width_is_derivable =
      splice.valid && removed_count == 1 && inserted_count == 1 &&
      start_line < cached_visual_line_columns_.size() && start_line < lines.size() &&
      cached_visual_line_columns_[start_line].facts().plain_ascii &&
      util::FirstNonAsciiOrByte(splice.inserted_text, '\t') == splice.inserted_text.size();

  std::vector<PackedLineWidth>& inserted_columns = inserted_columns_scratch_;
  inserted_columns.clear();
  inserted_columns.reserve(inserted_count);
  if (inline_width_is_derivable) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthSpliceUpdates);
#ifndef NDEBUG
    ++visual_column_splice_derived_count_;
#endif
    inserted_columns.push_back(PackedLineWidth::From(LineLayoutFacts{
        .visual_columns = lines.LineLength(start_line),
        .plain_ascii = true,
        .known = true,
    }));
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthFullMeasures, inserted_count);
    for (std::size_t i = 0; i < inserted_count && start_line + i < lines.size(); ++i) {
      inserted_columns.push_back(
          PackedLineWidth::From(TextLayout::MeasureLineFacts(lines[start_line + i], tab_size)));
    }
  }

  const std::size_t clamped_start = std::min(start_line, cached_visual_line_columns_.size());
  const std::size_t erase_end =
      std::min(clamped_start + removed_count, cached_visual_line_columns_.size());
  if (removed_count == inserted_columns.size() &&
      erase_end == clamped_start + removed_count) {
    // In-place fast path: the edit did not change the logical-line count, so no
    // suffix line's width entry moves. Overwrite the affected entries directly
    // instead of an erase+insert that memmoves the O(suffix) tail twice. This
    // is the dominant keystroke (an in-line edit), matching the wrapped-row
    // cache's in-place path.
    for (std::size_t i = 0; i < inserted_columns.size(); ++i) {
      cached_visual_line_columns_[clamped_start + i] = inserted_columns[i];
    }
#ifndef NDEBUG
    ++visual_column_incremental_inplace_count_;
#endif
  } else {
    cached_visual_line_columns_.erase(
        cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
        cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(erase_end));
    cached_visual_line_columns_.insert(
        cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
        inserted_columns.begin(), inserted_columns.end());
  }

  // Maintain the memoized widest line rather than dropping it. Dropping it costs
  // a whole-document rescan of the width table on the next reader -- and an edit
  // that WIDENS lines was the common way to trigger that, so commenting out 1000
  // lines of a 50k-line file measured 16 rescans of 50k entries each (800k), for
  // a new maximum that was sitting in `inserted_columns` the whole time.
  //
  // Exactly one case genuinely needs the rescan: the widest line itself was
  // rewritten or removed. Its old width is gone and the runner-up is not recorded
  // anywhere, so nothing here can name the new maximum. Every other edit leaves
  // the old maximum standing, and a line the edit did not touch cannot have
  // shrunk -- so the maximum can only grow, and it grows to the widest inserted
  // line, which is known without reading any other line.
  const std::size_t affected_end = clamped_start + removed_count;
  const bool max_line_replaced = cached_max_visual_columns_line_index_.has_value() &&
                                 *cached_max_visual_columns_line_index_ >= clamped_start &&
                                 *cached_max_visual_columns_line_index_ < affected_end;
  if (!cached_max_visual_columns_.has_value() ||
      !cached_max_visual_columns_line_index_.has_value() || max_line_replaced) {
    cached_max_visual_columns_.reset();
    cached_max_visual_columns_line_index_.reset();
  } else {
    if (*cached_max_visual_columns_line_index_ >= affected_end) {
      const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(inserted_columns.size()) -
                                   static_cast<std::ptrdiff_t>(removed_count);
      *cached_max_visual_columns_line_index_ = static_cast<std::size_t>(
          static_cast<std::ptrdiff_t>(*cached_max_visual_columns_line_index_) + delta);
    }
    for (std::size_t i = 0; i < inserted_columns.size(); ++i) {
      if (inserted_columns[i].visual_columns() > *cached_max_visual_columns_) {
        cached_max_visual_columns_ = inserted_columns[i].visual_columns();
        cached_max_visual_columns_line_index_ = clamped_start + i;
      }
    }
  }
  cached_max_visual_columns_content_revision_ = content_revision;
}

void TextLayoutCache::ClearVisibleLineAndMaxColumns() {
  // The plain-ASCII prefix memo is keyed by (line, content revision), which
  // covers every edit -- but a viewport that is handed a DIFFERENT document can
  // see a revision number repeat, and this is the wipe that runs on that path.
  plain_prefix_memo_ = {};
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_content_revision_ = 0;
  // Retire, don't destroy. This runs from UpdateVisualColumnCacheAfterEdit
  // whenever the per-line width table cannot be spliced, which is an ordinary
  // edit -- and it runs AFTER InvalidateVisibleLineCacheFrom has already retired
  // that edit's rows, so clearing the pool here handed the buffers back to the
  // allocator one call before the repaint asked for them again. `InvalidateAll`
  // is the wipe that actually releases them.
  RetireVisibleLineLayouts();
  visible_line_cache_.clear();
  visible_line_lru_head_ = nullptr;
  visible_line_lru_tail_ = nullptr;
}

void TextLayoutCache::RetireVisibleLineLayouts() {
  for (auto& [key, entry] : visible_line_cache_) {
    if (visible_line_layout_pool_.size() >= kVisibleLineCacheLimit) {
      break;
    }
    visible_line_layout_pool_.push_back(std::move(entry.layout));
  }
}

void TextLayoutCache::InvalidateVisibleLineCacheFrom(std::size_t start_line) {
  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (it->first.line_index >= start_line) {
      VisibleLineLruUnlink(it->second);
      // Retire the layout instead of destroying it; see
      // visible_line_layout_pool_. Over the cap it is dropped, which is the plain
      // erase this used to be.
      if (visible_line_layout_pool_.size() < kVisibleLineCacheLimit) {
        visible_line_layout_pool_.push_back(std::move(it->second.layout));
      }
      it = visible_line_cache_.erase(it);
      continue;
    }
    ++it;
  }
}

bool TextLayoutCache::has_wrapped_line_row_offsets(std::size_t lines_size) const {
  return wrapped_line_row_offsets_.size() == lines_size;
}

void TextLayoutCache::DropWrappedRowLayouts() {
  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  wrapped_row_layouts_tab_size_ = 0;
  wrapped_row_layouts_visible_columns_ = 0;
  wrapped_row_layouts_layout_shape_revision_ = 0;
  wrapped_row_layouts_soft_wrap_ = false;
  wrapped_row_layouts_folding_model_ = nullptr;
  wrapped_row_layouts_fold_revision_ = 0;
  wrapped_row_layouts_content_revision_ = 0;
  wrapped_row_layouts_trivial_ = false;
  wrapped_row_layouts_fold_no_wrap_ = false;
}

std::size_t TextLayoutCache::ApproximateResidentBytes() const {
  // libstdc++ allocates a deque in fixed 512-byte chunks (or one element, for a
  // larger element), so an element count converts to bytes exactly enough for a
  // ceiling to be chosen from it. The map node overhead is the conventional
  // estimate: one node per element plus one bucket pointer.
  constexpr std::size_t kDequeChunkBytes = 512;
  const auto deque_bytes = [](std::size_t elements, std::size_t element_size) {
    if (elements == 0) {
      return std::size_t{0};
    }
    const std::size_t per_chunk = std::max<std::size_t>(1, kDequeChunkBytes / element_size);
    return ((elements + per_chunk - 1) / per_chunk) * per_chunk * element_size;
  };

  std::size_t bytes = 0;
  bytes += deque_bytes(cached_visual_line_columns_.size(), sizeof(PackedLineWidth));
  bytes += inserted_columns_scratch_.capacity() * sizeof(PackedLineWidth);
  bytes += wrapped_row_layouts_.capacity() * sizeof(WrappedRow);
  bytes += wrapped_line_row_offsets_.capacity() * sizeof(std::size_t);
  bytes += line_window_scratch_.capacity();
  bytes += visible_line_cache_.bucket_count() * sizeof(void*);
  for (const auto& [key, entry] : visible_line_cache_) {
    // The recency order is intrusive now (two pointers inside the entry, counted
    // by sizeof(VisibleLineCacheEntry)), so there is no side container to add.
    bytes += sizeof(VisibleLineCacheKey) + sizeof(VisibleLineCacheEntry) + sizeof(void*) * 2;
    bytes += entry.layout.text.capacity();
    bytes += entry.layout.source_columns.capacity() * sizeof(std::size_t);
    bytes += entry.layout.text_offsets.capacity() * sizeof(std::size_t);
  }
  // Retired layouts hold their buffers until a miss claims them, so they are
  // resident and belong in the number this reports.
  for (const LayoutLine& layout : visible_line_layout_pool_) {
    bytes += sizeof(LayoutLine);
    bytes += layout.text.capacity();
    bytes += layout.source_columns.capacity() * sizeof(std::size_t);
    bytes += layout.text_offsets.capacity() * sizeof(std::size_t);
  }
  return bytes;
}

void TextLayoutCache::InvalidateAll() {
  // Delegate both halves rather than repeating them. InvalidateAll had drifted
  // from its own contract: it documented "wipes every cache + every cache key"
  // but left visible_line_cache_/_order_ populated, so "every cache" now
  // actually means every cache.
  ClearVisibleLineAndMaxColumns();
  DropWrappedRowLayouts();
  // The one caller that means "release it all". Everything else drops the cache
  // but keeps the buffers for the repaint that follows.
  visible_line_layout_pool_.clear();
  visible_line_layout_pool_.shrink_to_fit();
}

}  // namespace microide::editor
