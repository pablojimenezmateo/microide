#include "editor/TextLayoutCache.h"

#include <algorithm>

#include "editor/FoldingModel.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

const LayoutLine& TextLayoutCache::VisibleLineLayoutRefCached(LineSpan lines,
                                                              std::size_t line_index,
                                                              std::size_t horizontal_scroll,
                                                              std::size_t visible_columns,
                                                              std::size_t tab_size) const {
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
  LayoutLine layout =
      TextLayout::BuildVisibleLine(lines[line_index], horizontal_scroll, visible_columns, tab_size);
  if (visible_line_cache_.size() >= kVisibleLineCacheLimit) {
    visible_line_cache_.erase(visible_line_cache_order_.front());
    visible_line_cache_order_.pop_front();
  }
  const auto [it, _] = visible_line_cache_.emplace(cache_key, std::move(layout));
  visible_line_cache_order_.push_back(cache_key);
  return it->second;
}

LayoutLine TextLayoutCache::VisibleLineLayoutCached(LineSpan lines,
                                                    std::size_t line_index,
                                                    std::size_t horizontal_scroll,
                                                    std::size_t visible_columns,
                                                    std::size_t tab_size) const {
  return VisibleLineLayoutRefCached(lines, line_index, horizontal_scroll, visible_columns,
                                    tab_size);
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
      wrapped_row_layouts_fold_revision_ ==
          (folding_model != nullptr ? folding_model->revision() : 0)) {
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
        folding_model != nullptr ? folding_model->revision() : 0;
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
      folding_model != nullptr ? folding_model->revision() : 0;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
}

bool TextLayoutCache::UpdateWrappedRowsAfterEdit(
    std::size_t start_line, std::size_t removed_count,
    const std::vector<std::string>& inserted_lines, LineSpan lines,
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
          (folding_model != nullptr ? folding_model->revision() : 0)) {
    return false;
  }

  // The table must be in sync with the *pre-edit* buffer: one offset entry per
  // pre-edit logical line, contiguous rows per line (pure soft-wrap has no
  // hidden lines). old_line_count is derived from the new buffer + edit deltas.
  const std::size_t inserted_count = inserted_lines.size();
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
    cached_visual_line_columns_.assign(lines.size(), 0);
    for (std::size_t index = 0; index < lines.size(); ++index) {
      cached_visual_line_columns_[index] =
          TextLayout::VisualColumnForTextColumn(lines[index], lines[index].size(), tab_size);
    }
  }

  std::size_t max_columns = 0;
  std::size_t max_line = 0;
  for (std::size_t index = 0; index < cached_visual_line_columns_.size(); ++index) {
    if (cached_visual_line_columns_[index] >= max_columns) {
      max_columns = cached_visual_line_columns_[index];
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
    std::size_t start_line, std::size_t removed_count,
    const std::vector<std::string>& inserted_lines, LineSpan lines,
    std::size_t tab_size, std::uint64_t content_revision) {
  if (cached_max_visual_columns_tab_size_ != tab_size ||
      cached_visual_line_columns_.size() != lines.size() - inserted_lines.size() + removed_count) {
    // Reset only the visual-column + visible-line caches here. The wrapped-row
    // table is content-guarded separately (content_revision) and updated
    // incrementally by UpdateWrappedRowsAfterEdit, so it must not be wiped on
    // this hot path — doing so forced a full O(document) re-wrap per keystroke
    // under soft wrap.
    ClearVisibleLineAndMaxColumns();
    return;
  }

  std::vector<std::size_t> inserted_columns;
  inserted_columns.reserve(inserted_lines.size());
  for (const std::string& line : inserted_lines) {
    inserted_columns.push_back(TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size));
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
      std::any_of(inserted_columns.begin(), inserted_columns.end(), [&](std::size_t width) {
        return !cached_max_visual_columns_.has_value() || width >= *cached_max_visual_columns_;
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

void TextLayoutCache::InvalidateAll() {
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_content_revision_ = 0;
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

}  // namespace microide::editor
