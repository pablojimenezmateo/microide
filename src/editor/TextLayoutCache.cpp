#include "editor/TextLayoutCache.h"

#include <algorithm>
#include <cassert>

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
std::size_t FirstNonPlainAsciiByteInPrefix(LineSpan lines, std::size_t index, std::size_t limit,
                                           std::string& scratch) {
  std::size_t offset = 0;
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

const LayoutLine& TextLayoutCache::VisibleLineLayoutRefCached(LineSpan lines,
                                                              std::size_t line_index,
                                                              std::size_t horizontal_scroll,
                                                              std::size_t visible_columns,
                                                              std::size_t tab_size,
                                                              std::uint64_t content_revision) const {
  ++visible_line_queries_;
  const VisibleLineCacheKey cache_key{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll,
      .visible_columns = visible_columns,
      .tab_size = tab_size,
  };
  if (const auto it = visible_line_cache_.find(cache_key); it != visible_line_cache_.end()) {
    ++visible_line_hits_;
    return it->second;
  }
  // Hand the builder what this cache already knows about the line, so a miss does
  // not re-walk it for its visual width nor step code points to reach the first
  // visible cell. Both are O(line), and on a line with no newlines in it that is
  // the dominant cost of rendering a row (TD-2026-08-05-132, item 1).
  const LineLayoutFacts facts =
      LineFactsIfCurrent(lines.size(), line_index, tab_size, content_revision);
#ifndef NDEBUG
  // The table is only as good as the invariant that every content edit either
  // splices it or drops it. Cross-check it here, where a stale entry would
  // silently mislay a row's glyphs rather than merely mis-size the scrollbar.
  // Bounded by line length so the check costs nothing on the pathological lines
  // this whole path exists for.
  if (facts.known && lines.LineLength(line_index) <= 4096) {
    const LineLayoutFacts measured = TextLayout::MeasureLineFacts(lines[line_index], tab_size);
    assert(measured.visual_columns == facts.visual_columns &&
           measured.plain_ascii == facts.plain_ascii &&
           "per-line width table went stale against the buffer");
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
    const std::size_t plain_prefix_end =
        facts.plain_ascii ? line_length
                          : FirstNonPlainAsciiByteInPrefix(lines, line_index, probe,
                                                           line_window_scratch_);
    if (!facts.plain_ascii) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                                  std::min(plain_prefix_end, probe));
    }
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
    auto node = visible_line_cache_.extract(visible_line_cache_order_.front());
    visible_line_cache_order_.pop_front();
    if (!node.empty()) {
      build_into(node.mapped());
      node.key() = cache_key;
      util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);
      const auto result = visible_line_cache_.insert(std::move(node));
      visible_line_cache_order_.push_back(cache_key);
      return result.position->second;
    }
  }
  LayoutLine layout;
  build_into(layout);
  const auto [it, _] = visible_line_cache_.emplace(cache_key, std::move(layout));
  visible_line_cache_order_.push_back(cache_key);
  return it->second;
}


void TextLayoutCache::WrapSingleLine(std::string_view line_text,
                                     std::size_t line_index,
                                     std::size_t tab_size,
                                     std::size_t wrap_columns,
                                     std::vector<WrappedRow>& out) {
  if (line_text.empty()) {
    out.push_back(WrappedRow{line_index, 0, 0, 0});
    return;
  }

  // Hanging indent: continuation rows of this line render aligned under the
  // line's leading whitespace. Compute that indent once, clamped so a deep
  // indent never collapses continuation rows to a degenerate width.
  std::size_t hanging_indent = 0;
  {
    std::size_t indent_visual = 0;
    for (std::size_t k = 0; k < line_text.size(); ++k) {
      const char ch = line_text[k];
      if (ch != ' ' && ch != '\t') {
        break;
      }
      indent_visual = TextLayout::AdvanceVisualColumn(indent_visual, ch, tab_size);
    }
    hanging_indent = std::min(indent_visual, wrap_columns / 2);
  }

  // Single pass: walk the line tracking the visual column, the last whitespace
  // break opportunity, and the current row's text start. Break before any
  // character that would push the row past the row's available width; prefer
  // breaking after the most recent whitespace if one is available inside the
  // current row. Hard-break inside a long word only when no whitespace fits.
  // Continuation rows (row_start_visual > 0) lose `hanging_indent` columns of
  // width to the rendered indent.
  std::size_t row_start_visual = 0;
  std::size_t row_start_text = 0;
  std::size_t last_break_visual = 0;
  std::size_t last_break_text = 0;
  std::size_t visual = 0;
  std::size_t i = 0;
  const std::size_t line_size = line_text.size();
  while (i < line_size) {
    const char ch = line_text[i];
    const std::size_t seq_len = util::Utf8SequenceLength(line_text, i);
    const std::size_t next_visual = TextLayout::AdvanceVisualColumn(visual, ch, tab_size);
    const std::size_t effective_wrap =
        wrap_columns - (row_start_visual == 0 ? 0 : hanging_indent);

    if (next_visual - row_start_visual > effective_wrap && i > row_start_text) {
      std::size_t break_visual;
      std::size_t break_text;
      if (last_break_text > row_start_text) {
        break_visual = last_break_visual;
        break_text = last_break_text;
      } else {
        break_visual = visual;
        break_text = i;
      }
      out.push_back(WrappedRow{line_index, row_start_visual, break_visual,
                               row_start_visual == 0 ? 0 : hanging_indent});
      row_start_visual = break_visual;
      row_start_text = break_text;
      last_break_visual = row_start_visual;
      last_break_text = row_start_text;
      visual = break_visual;
      i = break_text;
      continue;
    }

    visual = next_visual;
    i += seq_len;
    if (ch == ' ' || ch == '\t') {
      last_break_visual = visual;
      last_break_text = i;
    }
  }
  out.push_back(WrappedRow{line_index, row_start_visual, visual,
                           row_start_visual == 0 ? 0 : hanging_indent});
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
  std::size_t last = clamped_first;
  while (last + 1 < row_count && wrapped_row_layouts_[last + 1].line_index == owner_line) {
    ++last;
  }
  return {clamped_first, last};
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

std::size_t TextLayoutCache::MaxVisualColumns(LineSpan lines,
                                              std::size_t tab_size,
                                              std::uint64_t content_revision) const {
  if (cached_max_visual_columns_.has_value() && cached_max_visual_columns_tab_size_ == tab_size &&
      cached_max_visual_columns_content_revision_ == content_revision) {
    return *cached_max_visual_columns_;
  }

  if (cached_max_visual_columns_tab_size_ != tab_size ||
      cached_visual_line_columns_.size() != lines.size()) {
    util::PerformanceTrace::Scope s("TextLayoutCache::BuildVisualLineColumns");
    // One reason per build, so the three reasons sum to the build count. Cold is
    // checked first because an empty table is simultaneously a tab-size and a
    // line-count mismatch, and attributing it to either would hide the real
    // repeat-build shapes (TD-2026-08-06-138).
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds);
    if (cached_visual_line_columns_.empty()) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildCold);
    } else if (cached_max_visual_columns_tab_size_ != tab_size) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildTabSize);
    } else {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthRebuildLineCount);
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

  const bool max_line_erased =
      cached_max_visual_columns_line_index_.has_value() &&
      *cached_max_visual_columns_line_index_ >= clamped_start &&
      *cached_max_visual_columns_line_index_ < clamped_start + removed_count;
  const bool candidate_expands_max =
      std::any_of(inserted_columns.begin(), inserted_columns.end(), [&](PackedLineWidth width) {
        return !cached_max_visual_columns_.has_value() ||
               width.visual_columns() >= *cached_max_visual_columns_;
      });
  if (max_line_erased || candidate_expands_max || !cached_max_visual_columns_.has_value()) {
    cached_max_visual_columns_.reset();
    cached_max_visual_columns_line_index_.reset();
  } else if (cached_max_visual_columns_line_index_.has_value() &&
             *cached_max_visual_columns_line_index_ >= clamped_start) {
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(inserted_columns.size()) -
                                 static_cast<std::ptrdiff_t>(removed_count);
    *cached_max_visual_columns_line_index_ = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(*cached_max_visual_columns_line_index_) + delta);
  }
  cached_max_visual_columns_content_revision_ = content_revision;
}

void TextLayoutCache::ClearVisibleLineAndMaxColumns() {
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_content_revision_ = 0;
  visible_line_cache_.clear();
  visible_line_cache_order_.clear();
}

void TextLayoutCache::InvalidateVisibleLineCacheFrom(std::size_t start_line) {
  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (it->first.line_index >= start_line) {
      it = visible_line_cache_.erase(it);
    } else {
      ++it;
    }
  }
  visible_line_cache_order_.erase(
      std::remove_if(visible_line_cache_order_.begin(), visible_line_cache_order_.end(),
                     [&](const VisibleLineCacheKey& key) { return key.line_index >= start_line; }),
      visible_line_cache_order_.end());
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

void TextLayoutCache::InvalidateAll() {
  // Delegate both halves rather than repeating them. InvalidateAll had drifted
  // from its own contract: it documented "wipes every cache + every cache key"
  // but left visible_line_cache_/_order_ populated, so "every cache" now
  // actually means every cache.
  ClearVisibleLineAndMaxColumns();
  DropWrappedRowLayouts();
}

}  // namespace microide::editor
