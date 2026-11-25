#include "editor/TextLayoutCache.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iterator>

#include "editor/FoldingModel.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

constexpr std::size_t kPrefixScanChunkBytes = 4096;

constexpr std::size_t kMaxLineWindowBytes = 64u * 1024u;

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

}

std::size_t TextLayoutCache::PlainAsciiPrefixEnd(LineSpan lines,
                                                 std::size_t line_index,
                                                 std::size_t probe,
                                                 std::uint64_t content_revision) const {
  return PlainAsciiPrefixEndInto(lines, line_index, probe, content_revision, line_window_scratch_);
}

bool TextLayoutCache::PlainAsciiThroughColumn(LineSpan lines,
                                              std::size_t line_index,
                                              std::size_t column,
                                              std::uint64_t content_revision) const {
  if (line_index >= lines.size()) {
    return false;
  }
  const std::size_t probe = std::min(column, lines.LineLength(line_index));
  return PlainAsciiPrefixEndInto(lines, line_index, probe, content_revision,
                                 column_scan_scratch_) >= probe;
}

std::size_t TextLayoutCache::PlainAsciiPrefixEndInto(LineSpan lines,
                                                     std::size_t line_index,
                                                     std::size_t probe,
                                                     std::uint64_t content_revision,
                                                     std::string& scratch) const {
  if (probe == 0) {
    return 0;
  }
  const std::size_t line_length = lines.LineLength(line_index);
  for (PlainPrefixMemo& memo : plain_prefix_memo_) {
    if (!memo.valid || memo.line_index != line_index || memo.line_length != line_length ||
        memo.content_revision != content_revision) {
      continue;
    }
    if (memo.first_non_plain < memo.scanned_through) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorPlainPrefixMemoHits);
      return std::min(memo.first_non_plain, probe);
    }
    if (probe <= memo.scanned_through) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorPlainPrefixMemoHits);
      return probe;
    }
    util::AddPerformanceCounter(util::PerfCounterId::EditorPlainPrefixMemoExtends);
    const std::size_t hit = FirstNonPlainAsciiByteInRange(lines, line_index, memo.scanned_through,
                                                          probe, scratch);
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                                std::min(hit, probe) - memo.scanned_through);
    memo.scanned_through = probe;
    memo.first_non_plain = hit;
    return std::min(hit, probe);
  }

  util::AddPerformanceCounter(util::PerfCounterId::EditorPlainPrefixMemoCold);
  const std::size_t hit =
      FirstNonPlainAsciiByteInRange(lines, line_index, 0, probe, scratch);
  util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutPrefixBytesScanned,
                              std::min(hit, probe));
  PlainPrefixMemo& slot = plain_prefix_memo_[plain_prefix_memo_next_];
  plain_prefix_memo_next_ = (plain_prefix_memo_next_ + 1) % kPlainPrefixMemoSlots;
  slot = PlainPrefixMemo{line_index, line_length, content_revision, probe, hit, true};
  return hit;
}

void TextLayoutCache::CarryPlainPrefixMemosAcrossEdit(LineSpan lines,
                                                      std::size_t start_line,
                                                      std::size_t edit_column,
                                                      std::uint64_t old_revision,
                                                      std::uint64_t new_revision) const {
  for (PlainPrefixMemo& memo : plain_prefix_memo_) {
    if (!memo.valid || memo.content_revision != old_revision || memo.line_index > start_line) {
      continue;
    }
    if (memo.line_index < start_line) {
      memo.content_revision = new_revision;
      continue;
    }
    if (edit_column == kNoEditColumn || memo.line_index >= lines.size()) {
      memo.valid = false;
      continue;
    }
    if (memo.first_non_plain < memo.scanned_through && memo.first_non_plain < edit_column) {
      memo.scanned_through = memo.first_non_plain + 1;
    } else {
      memo.scanned_through = std::min(memo.scanned_through, edit_column);
      memo.first_non_plain = memo.scanned_through;
    }
    memo.line_length = lines.LineLength(memo.line_index);
    memo.content_revision = new_revision;
  }
}

const LayoutLine& TextLayoutCache::VisibleLineLayoutRefCached(LineSpan lines,
                                                              std::size_t line_index,
                                                              std::size_t horizontal_scroll,
                                                              std::size_t visible_columns,
                                                              std::size_t tab_size,
                                                              std::uint64_t content_revision,
                                                              LineLayoutFacts facts_hint) const {
  ++visible_line_queries_;
  RelinkVisibleLineLruIfDetached();
  if (content_revision != visible_line_cache_content_revision_) {
    InvalidateVisibleLineCacheFrom(0, content_revision);
  }
  const VisibleLineCacheKey cache_key{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll,
      .visible_columns = visible_columns,
      .tab_size = tab_size,
  };
  if (const auto it = visible_line_cache_.find(cache_key); it != visible_line_cache_.end()) {
    ++visible_line_hits_;
    VisibleLineLruTouch(it->second);
    return it->second.layout;
  }
  LineLayoutFacts facts =
      LineFactsIfCurrent(lines.size(), line_index, tab_size, content_revision);
  [[maybe_unused]] const bool facts_from_width_table = facts.known;
  if (!facts.known) {
    facts = facts_hint;
  }
#ifndef NDEBUG
  if (facts.known && lines.LineLength(line_index) <= 4096) {
    const LineLayoutFacts measured = TextLayout::MeasureLineFacts(lines[line_index], tab_size);
    assert((!facts_from_width_table || measured.visual_columns == facts.visual_columns) &&
           "per-line width table went stale against the buffer");
    assert((facts_from_width_table || measured.visual_columns == facts.visual_columns) &&
           "the wrapped-row table's width hint went stale against the buffer");
    assert((facts_from_width_table ? measured.plain_ascii == facts.plain_ascii
                                   : (!facts.plain_ascii || measured.plain_ascii)) &&
           "line layout facts claim plain ASCII for a line that is not");
  }
#endif
  TextLayout::VisibleLineWindow window;
  bool windowed = false;
  if (facts.known) {
    const std::size_t line_length = lines.LineLength(line_index);
    const std::size_t probe = std::min(horizontal_scroll, line_length);
    const std::size_t plain_prefix_end =
        facts.plain_ascii ? line_length
                          : PlainAsciiPrefixEnd(lines, line_index, probe, content_revision);
    const std::size_t start_byte = TextLayout::ComputeVisibleLineWindowStart(
        horizontal_scroll, line_length, plain_prefix_end);
    const std::size_t want =
        TextLayout::VisibleLineWindowBytes(start_byte, horizontal_scroll, visible_columns);
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
  const auto reuse_node = [&](auto node) -> const LayoutLine& {
    build_into(node.mapped().layout);
    node.mapped().key = cache_key;
    node.key() = cache_key;
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);
    const auto result = visible_line_cache_.insert(std::move(node));
    VisibleLineLruPushBack(result.position->second);
    return result.position->second.layout;
  };
  if (visible_line_cache_.size() >= kVisibleLineCacheLimit) {
    ++visible_line_evictions_;
    util::AddPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutEvictions);
    assert(visible_line_lru_.head != nullptr);
    VisibleLineCacheEntry& victim = *visible_line_lru_.head;
    VisibleLineLruUnlink(victim);
    if (auto node = visible_line_cache_.extract(victim.key); !node.empty()) {
      return reuse_node(std::move(node));
    }
  }
  if (!visible_line_node_pool_.empty()) {
    return reuse_node(visible_line_node_pool_.Take());
  }
  const auto [it, _] = visible_line_cache_.emplace(cache_key, VisibleLineCacheEntry{});
  it->second.key = cache_key;
  build_into(it->second.layout);
  VisibleLineLruPushBack(it->second);
  return it->second.layout;
}

void TextLayoutCache::VisibleLineLruUnlink(VisibleLineCacheEntry& entry) const {
  if (entry.lru_prev != nullptr) {
    entry.lru_prev->lru_next = entry.lru_next;
  } else if (visible_line_lru_.head == &entry) {
    visible_line_lru_.head = entry.lru_next;
  }
  if (entry.lru_next != nullptr) {
    entry.lru_next->lru_prev = entry.lru_prev;
  } else if (visible_line_lru_.tail == &entry) {
    visible_line_lru_.tail = entry.lru_prev;
  }
  entry.lru_prev = nullptr;
  entry.lru_next = nullptr;
}

void TextLayoutCache::VisibleLineLruPushBack(VisibleLineCacheEntry& entry) const {
  entry.lru_prev = visible_line_lru_.tail;
  entry.lru_next = nullptr;
  if (visible_line_lru_.tail != nullptr) {
    visible_line_lru_.tail->lru_next = &entry;
  } else {
    visible_line_lru_.head = &entry;
  }
  visible_line_lru_.tail = &entry;
}


void TextLayoutCache::WrapSingleLine(std::string_view line_text,
                                     std::size_t line_index,
                                     std::size_t tab_size,
                                     std::size_t wrap_columns,
                                     std::vector<WrappedRow>& out) {
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
  const bool has_any_collapsed_fold =
      folding_model != nullptr && folding_model->has_any_collapsed_fold();
  const bool trivial_now = !soft_wrap && !has_any_collapsed_fold;

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
          (folding_model != nullptr ? folding_model->layout_revision() : 0)) {
    return;
  }

  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);
  const auto record_build_key = [&]() {
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
  };

  if (!soft_wrap && !has_any_collapsed_fold) {
    wrapped_row_layouts_trivial_ = true;
    wrapped_row_layouts_fold_no_wrap_ = false;
    util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits, 0);
    record_build_key();
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
  record_build_key();
}

#ifndef NDEBUG
void TextLayoutCache::VerifyWrappedRowsAgainstFullRewrap(LineSpan lines,
                                                         std::size_t line_index,
                                                         std::size_t tab_size,
                                                         std::size_t wrap_columns) const {
  if (line_index + 1 >= wrapped_line_row_offsets_.size() ||
      lines.LineLength(line_index) > 64u * 1024u) {
    return;
  }
  const std::size_t begin = wrapped_line_row_offsets_[line_index];
  const std::size_t end = wrapped_line_row_offsets_[line_index + 1];
  assert(begin <= end && end <= wrapped_row_layouts_.size() &&
         "spliced row range must stay inside the table");
  std::size_t index = begin;
  bool matched = true;
  TextLayout::WrapLineSegments(lines[line_index], tab_size, wrap_columns,
                               [&](std::size_t visual_start, std::size_t visual_end,
                                   std::size_t indent) {
                                 if (index >= end) {
                                   matched = false;
                                   return;
                                 }
                                 const WrappedRow& row = wrapped_row_layouts_[index++];
                                 matched = matched && row.line_index == line_index &&
                                           row.visual_start == visual_start &&
                                           row.visual_end == visual_end && row.indent == indent;
                               });
  assert(matched && index == end &&
         "incrementally spliced wrapped rows disagree with a full re-wrap of the line");
}
#endif

bool TextLayoutCache::UpdateWrappedRowsAfterEdit(
    std::size_t start_line, std::size_t removed_count, std::size_t inserted_count,
    LineSpan lines,
    std::size_t tab_size, std::size_t visible_columns, bool soft_wrap,
    const FoldingModel* folding_model, std::uint64_t layout_shape_revision,
    std::uint64_t content_revision, std::size_t edit_column) const {
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

  if (!DescribesTheTextJustBefore(wrapped_row_layouts_content_revision_, content_revision) ||
      lines.size() + removed_count < inserted_count) {
    return false;
  }
  const std::size_t old_line_count = lines.size() + removed_count - inserted_count;
  if (wrapped_line_row_offsets_.size() != old_line_count || old_line_count == 0 ||
      start_line + removed_count > old_line_count) {
    return false;
  }

  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns);
  const std::size_t total_rows = wrapped_row_layouts_.size();
  (void)edit_column;
  const std::size_t first_row =
      start_line < old_line_count ? wrapped_line_row_offsets_[start_line] : total_rows;
  const std::size_t removed_end_line = start_line + removed_count;
  const std::size_t removed_rows_end =
      removed_end_line < old_line_count ? wrapped_line_row_offsets_[removed_end_line] : total_rows;
  if (first_row > removed_rows_end || removed_rows_end > total_rows) {
    return false;
  }

  std::size_t splice_first_row = first_row;
  std::size_t resume_visual = 0;
  std::size_t resume_byte = 0;
  std::size_t hanging_indent = 0;
  bool resumed = false;
  if (inserted_count == 1 && removed_count == 1 && edit_column != kNoEditColumn &&
      first_row < removed_rows_end &&
      PlainAsciiThroughColumn(lines, start_line, edit_column, content_revision)) {
    const std::size_t edit_visual = edit_column;
    std::size_t lo = first_row;
    std::size_t hi = removed_rows_end;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (wrapped_row_layouts_[mid].visual_start + wrap_columns < edit_visual) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo > first_row) {
      splice_first_row = lo;
      resume_visual = wrapped_row_layouts_[lo].visual_start;
      resume_byte = resume_visual;
      resumed = true;
    }
  }

  std::vector<WrappedRow>& new_rows = wrapped_row_edit_scratch_;
  new_rows.clear();
  new_rows.reserve(std::max(inserted_count, removed_rows_end - splice_first_row));
  std::vector<std::size_t>& inserted_offsets = wrapped_row_edit_offsets_scratch_;
  inserted_offsets.clear();
  inserted_offsets.reserve(inserted_count);
  if (resumed) {
    const std::size_t line_length = lines.LineLength(start_line);
    hanging_indent = TextLayout::HangingIndentFor(
        lines.LineWindow(start_line, 0, std::min(line_length, wrap_columns + 1),
                         wrapped_row_tail_scratch_),
        tab_size, wrap_columns);
    inserted_offsets.push_back(0);
    TextLayout::WrapLineSegmentsFrom(
        lines.LineWindow(start_line, resume_byte, line_length - resume_byte,
                         wrapped_row_tail_scratch_),
        tab_size, wrap_columns, resume_visual, hanging_indent,
        [&new_rows, start_line](std::size_t visual_start, std::size_t visual_end,
                                std::size_t indent) {
          new_rows.push_back(WrappedRow{start_line, visual_start, visual_end, indent});
        });
  } else {
    for (std::size_t i = 0; i < inserted_count; ++i) {
      inserted_offsets.push_back(new_rows.size());
      WrapSingleLine(lines[start_line + i], start_line + i, tab_size, wrap_columns, new_rows);
    }
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(inserted_count) - static_cast<std::ptrdiff_t>(removed_count);
  const std::size_t removed_rows = removed_rows_end - splice_first_row;
  const auto absolute_line_offset = [&](std::size_t i) {
    return resumed ? first_row : first_row + inserted_offsets[i];
  };

  if (line_delta == 0 && new_rows.size() == removed_rows) {
    std::copy(new_rows.begin(), new_rows.end(),
              wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(splice_first_row));
    for (std::size_t i = 0; i < inserted_count; ++i) {
      wrapped_line_row_offsets_[start_line + i] = absolute_line_offset(i);
    }
    wrapped_row_layouts_content_revision_ = content_revision;
#ifndef NDEBUG
    ++wrapped_row_incremental_inplace_count_;
    VerifyWrappedRowsAgainstFullRewrap(lines, start_line, tab_size, wrap_columns);
#endif
    return true;
  }

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
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(splice_first_row),
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(removed_rows_end));
  wrapped_row_layouts_.insert(
      wrapped_row_layouts_.begin() + static_cast<std::ptrdiff_t>(splice_first_row),
      new_rows.begin(), new_rows.end());

  const std::ptrdiff_t row_delta = static_cast<std::ptrdiff_t>(new_rows.size()) -
                                   static_cast<std::ptrdiff_t>(removed_rows);
  const std::size_t new_line_count = lines.size();
  std::vector<std::size_t>& new_offsets = wrapped_line_row_offsets_scratch_;
  new_offsets.clear();
  new_offsets.reserve(new_line_count);
  for (std::size_t l = 0; l < start_line; ++l) {
    new_offsets.push_back(wrapped_line_row_offsets_[l]);
  }
  for (std::size_t i = 0; i < inserted_count; ++i) {
    new_offsets.push_back(absolute_line_offset(i));
  }
  for (std::size_t l = removed_end_line; l < old_line_count; ++l) {
    new_offsets.push_back(static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(wrapped_line_row_offsets_[l]) + row_delta));
  }
  if (new_offsets.size() != new_line_count || wrapped_row_layouts_.empty()) {
    return false;
  }
  wrapped_line_row_offsets_.swap(new_offsets);
  wrapped_row_layouts_content_revision_ = content_revision;
#ifndef NDEBUG
  VerifyWrappedRowsAgainstFullRewrap(lines, start_line, tab_size, wrap_columns);
#endif
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
  const std::size_t owner_line = wrapped_row_layouts_[clamped_first].line_index;
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
  if (visual_column_block_max_.size() == VisualColumnBlockCount()) {
    for (std::size_t block_index = 0; block_index < visual_column_block_max_.size();
         ++block_index) {
      const std::size_t block_start = block_index * kVisualColumnBlockLines;
      const std::size_t block_end =
          std::min(block_start + kVisualColumnBlockLines, cached_visual_line_columns_.size());
      std::size_t widest = 0;
      for (std::size_t index = block_start; index < block_end; ++index) {
        widest = std::max(widest, cached_visual_line_columns_[index].visual_columns());
      }
      assert(visual_column_block_max_[block_index].columns == widest &&
             "block maximum disagrees with the width-table entries it summarises");
    }
  }
}
#endif

std::string TextLayoutCache::VerifyAgainstFreshBuildForTesting(LineSpan lines,
                                                               std::size_t tab_size,
                                                               std::size_t visible_columns,
                                                               bool soft_wrap,
                                                               const FoldingModel* folding_model,
                                                               std::uint64_t layout_shape_revision,
                                                               std::uint64_t content_revision) const {
  const auto at = [](std::string_view what, std::size_t index) {
    return std::string(what) + " " + std::to_string(index);
  };
  if (LineWidthsAreCurrent(lines.size(), tab_size, content_revision)) {
    std::size_t widest = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const LineLayoutFacts fresh = TextLayout::MeasureLineFacts(lines[index], tab_size);
      const LineLayoutFacts cached = cached_visual_line_columns_[index].facts();
      if (cached.visual_columns != fresh.visual_columns || cached.plain_ascii != fresh.plain_ascii) {
        return at("the width table claims current and disagrees with line", index) + ": " +
               std::to_string(cached.visual_columns) + " columns cached, " +
               std::to_string(fresh.visual_columns) + " measured";
      }
      widest = std::max(widest, fresh.visual_columns);
    }
    if (cached_max_visual_columns_.has_value() && *cached_max_visual_columns_ != widest) {
      return "the memoized widest line (" + std::to_string(*cached_max_visual_columns_) +
             ") disagrees with the lines (" + std::to_string(widest) + ")";
    }
    if (visual_column_block_max_.size() == VisualColumnBlockCount()) {
      for (std::size_t block = 0; block < visual_column_block_max_.size(); ++block) {
        std::size_t block_widest = 0;
        const std::size_t end = std::min((block + 1) * kVisualColumnBlockLines, lines.size());
        for (std::size_t index = block * kVisualColumnBlockLines; index < end; ++index) {
          block_widest = std::max(block_widest, cached_visual_line_columns_[index].visual_columns());
        }
        if (visual_column_block_max_[block].columns != block_widest) {
          return at("the width table's block maximum disagrees for block", block);
        }
      }
    }
  }
  const bool trivial_now = !soft_wrap && !(folding_model != nullptr && folding_model->has_any_collapsed_fold());
  const bool claims_current =
      !trivial_now && !wrapped_row_layouts_trivial_ &&
      wrapped_row_layouts_layout_shape_revision_ == layout_shape_revision &&
      wrapped_row_layouts_content_revision_ == content_revision && wrapped_row_layouts_tab_size_ == tab_size &&
      wrapped_row_layouts_visible_columns_ == visible_columns && wrapped_row_layouts_soft_wrap_ == soft_wrap &&
      wrapped_row_layouts_folding_model_ == folding_model &&
      wrapped_row_layouts_fold_revision_ == (folding_model != nullptr ? folding_model->layout_revision() : 0);
  if (claims_current) {
    TextLayoutCache fresh;
    fresh.EnsureWrappedRowLayouts(lines, tab_size, visible_columns, soft_wrap, folding_model,
                                  layout_shape_revision, content_revision);
    if (fresh.wrapped_row_layouts_fold_no_wrap_ != wrapped_row_layouts_fold_no_wrap_) {
      return "the wrapped-row table claims current and disagrees about fold-but-no-wrap";
    }
    if (fresh.wrapped_row_layouts_.size() != wrapped_row_layouts_.size()) {
      return "the wrapped-row table claims current with " + std::to_string(wrapped_row_layouts_.size()) +
             " rows; a fresh wrap has " + std::to_string(fresh.wrapped_row_layouts_.size());
    }
    for (std::size_t row = 0; row < wrapped_row_layouts_.size(); ++row) {
      const WrappedRow& a = wrapped_row_layouts_[row];
      const WrappedRow& b = fresh.wrapped_row_layouts_[row];
      if (a.line_index != b.line_index || a.visual_start != b.visual_start || a.visual_end != b.visual_end ||
          a.indent != b.indent) {
        return at("the wrapped-row table claims current and disagrees at row", row) + " (line " +
               std::to_string(a.line_index) + " " + std::to_string(a.visual_start) + ".." +
               std::to_string(a.visual_end) + " cached, line " + std::to_string(b.line_index) + " " +
               std::to_string(b.visual_start) + ".." + std::to_string(b.visual_end) + " fresh)";
      }
    }
    if (fresh.wrapped_line_row_offsets_ != wrapped_line_row_offsets_) {
      return "the wrapped-row table's line offsets claim current and disagree with a fresh wrap";
    }
  }
  if (!VisibleLineLruIsConsistentForTesting()) {
    return "the visible-line LRU is inconsistent with its map";
  }
  return {};
}

void TextLayoutCache::RebuildVisualColumnBlockMaxima() const {
  const std::size_t lines = cached_visual_line_columns_.size();
  visual_column_block_max_.assign(VisualColumnBlockCount(), VisualColumnBlockMax{});
  if (lines == 0) {
    return;
  }
  for (std::size_t index = 0; index < lines; ++index) {
    VisualColumnBlockMax& block = visual_column_block_max_[index / kVisualColumnBlockLines];
    const std::size_t columns = cached_visual_line_columns_[index].visual_columns();
    if (columns >= block.columns) {
      block.columns = static_cast<std::uint32_t>(columns);
      block.line = static_cast<std::uint32_t>(index);
    }
  }
}

void TextLayoutCache::RefreshVisualColumnBlocksFor(std::size_t first_line,
                                                   std::size_t last_line) const {
  const std::size_t lines = cached_visual_line_columns_.size();
  if (lines == 0 || visual_column_block_max_.size() != VisualColumnBlockCount()) {
    visual_column_block_max_.clear();
    return;
  }
  if (first_line >= lines) {
    return;
  }
  last_line = std::min(last_line, lines - 1);
  const std::size_t first_block = first_line / kVisualColumnBlockLines;
  const std::size_t last_block = last_line / kVisualColumnBlockLines;
  for (std::size_t block_index = first_block; block_index <= last_block; ++block_index) {
    const std::size_t block_start = block_index * kVisualColumnBlockLines;
    const std::size_t block_end = std::min(block_start + kVisualColumnBlockLines, lines);
    const std::size_t touched_begin = std::max(block_start, first_line);
    const std::size_t touched_end = std::min(block_end, last_line + 1);
    VisualColumnBlockMax& block = visual_column_block_max_[block_index];

    std::size_t widest = 0;
    std::size_t widest_line = touched_begin;
    for (std::size_t index = touched_begin; index < touched_end; ++index) {
      const std::size_t columns = cached_visual_line_columns_[index].visual_columns();
      if (columns >= widest) {
        widest = columns;
        widest_line = index;
      }
    }

    if (widest >= block.columns) {
      block.columns = static_cast<std::uint32_t>(widest);
      block.line = static_cast<std::uint32_t>(widest_line);
      continue;
    }
    if (block.line < touched_begin || block.line >= touched_end) {
      continue;
    }
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthBlockRescans);
    VisualColumnBlockMax refreshed;
    for (std::size_t index = block_start; index < block_end; ++index) {
      const std::size_t columns = cached_visual_line_columns_[index].visual_columns();
      if (columns >= refreshed.columns) {
        refreshed.columns = static_cast<std::uint32_t>(columns);
        refreshed.line = static_cast<std::uint32_t>(index);
      }
    }
    block = refreshed;
  }
}

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

  if (!LineWidthsAreCurrent(lines.size(), tab_size, content_revision)) {
    util::PerformanceTrace::Scope s("TextLayoutCache::BuildVisualLineColumns");
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
    if (parked_line_widths_.widths.has_value() && parked_line_widths_.tab_size == tab_size &&
        parked_line_widths_.content_revision == content_revision &&
        parked_line_widths_.widths->size() == lines.size()) {
      cached_visual_line_columns_.swap(*parked_line_widths_.widths);
    } else {
      cached_visual_line_columns_.assign(lines.size(), PackedLineWidth{});
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthFullMeasures, lines.size());
      for (std::size_t index = 0; index < lines.size(); ++index) {
        cached_visual_line_columns_[index] =
            PackedLineWidth::From(TextLayout::MeasureLineFacts(lines[index], tab_size));
      }
    }
    parked_line_widths_.Reset();
    line_width_parking_armed_ = false;
    RebuildVisualColumnBlockMaxima();
  }

  util::PerformanceTrace::Scope sm("TextLayoutCache::ScanVisualLineColumnsMax");
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxScans);
  if (visual_column_block_max_.size() != VisualColumnBlockCount()) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxScanLines,
                                cached_visual_line_columns_.size());
    RebuildVisualColumnBlockMaxima();
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxScanBlocks,
                                visual_column_block_max_.size());
  }
  std::size_t max_columns = 0;
  std::size_t max_line = 0;
  for (const VisualColumnBlockMax& block : visual_column_block_max_) {
    if (block.columns >= max_columns) {
      max_columns = block.columns;
      max_line = block.line;
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
      !DescribesTheTextJustBefore(cached_max_visual_columns_content_revision_, content_revision) ||
      lines.size() + removed_count < inserted_count ||
      cached_visual_line_columns_.size() != lines.size() - inserted_count + removed_count) {
    ClearVisibleLineAndMaxColumns();
    return;
  }

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
    for (std::size_t i = 0; i < inserted_columns.size(); ++i) {
      cached_visual_line_columns_[clamped_start + i] = inserted_columns[i];
    }
    if (!inserted_columns.empty()) {
      RefreshVisualColumnBlocksFor(clamped_start, clamped_start + inserted_columns.size() - 1);
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
    visual_column_block_max_.clear();
  }

  const std::size_t affected_end = clamped_start + removed_count;
  const bool max_line_replaced = cached_max_visual_columns_line_index_.has_value() &&
                                 *cached_max_visual_columns_line_index_ >= clamped_start &&
                                 *cached_max_visual_columns_line_index_ < affected_end;
  bool adopted_wider_replacement = false;
  if (max_line_replaced && cached_max_visual_columns_.has_value()) {
    std::size_t widest_inserted = 0;
    std::size_t widest_inserted_line = clamped_start;
    for (std::size_t i = 0; i < inserted_columns.size(); ++i) {
      if (inserted_columns[i].visual_columns() >= widest_inserted) {
        widest_inserted = inserted_columns[i].visual_columns();
        widest_inserted_line = clamped_start + i;
      }
    }
    if (!inserted_columns.empty() && widest_inserted >= *cached_max_visual_columns_) {
      cached_max_visual_columns_ = widest_inserted;
      cached_max_visual_columns_line_index_ = widest_inserted_line;
      adopted_wider_replacement = true;
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxKeptOnReplace);
    }
  }
  if (adopted_wider_replacement) {
  } else if (!cached_max_visual_columns_.has_value() ||
      !cached_max_visual_columns_line_index_.has_value() || max_line_replaced) {
    if (cached_max_visual_columns_.has_value()) {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxDroppedReplacedNarrower);
    } else {
      util::AddPerformanceCounter(util::PerfCounterId::EditorLineWidthMaxDroppedAlreadyAbsent);
    }
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

void TextLayoutCache::ParkLineWidthsForTabSizeChange() {
  if (!line_width_parking_armed_) {
    ClearVisibleLineAndMaxColumns();
    return;
  }
  ParkedLineWidths parked;
  parked.widths.swap(parked_line_widths_.widths);
  parked.tab_size = parked_line_widths_.tab_size;
  parked.content_revision = parked_line_widths_.content_revision;
  if (!cached_visual_line_columns_.empty() && cached_max_visual_columns_tab_size_ != 0) {
    if (!parked.widths.has_value()) {
      parked.widths.emplace();
    }
    parked.widths->swap(cached_visual_line_columns_);
    parked.tab_size = cached_max_visual_columns_tab_size_;
    parked.content_revision = cached_max_visual_columns_content_revision_;
  }
  ClearVisibleLineAndMaxColumns();
  parked_line_widths_.widths.swap(parked.widths);
  parked_line_widths_.tab_size = parked.tab_size;
  parked_line_widths_.content_revision = parked.content_revision;
}

void TextLayoutCache::ClearVisibleLineAndMaxColumns() {
  parked_line_widths_.Reset();
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  visual_column_block_max_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_content_revision_ = 0;
  RetireVisibleLineNodes();
  visible_line_lru_.head = nullptr;
  visible_line_lru_.tail = nullptr;
}

void TextLayoutCache::RetireVisibleLineNodes() {
  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (visible_line_node_pool_.size() >= kVisibleLineCacheLimit) {
      break;
    }
    const auto next = std::next(it);
    visible_line_node_pool_.Retire(visible_line_cache_.extract(it));
    it = next;
  }
  visible_line_cache_.clear();
}

bool TextLayoutCache::VisibleLineLruIsConsistentForTesting() const {
  RelinkVisibleLineLruIfDetached();
  std::size_t forward = 0;
  const VisibleLineCacheEntry* previous = nullptr;
  for (const VisibleLineCacheEntry* entry = visible_line_lru_.head; entry != nullptr;
       entry = entry->lru_next) {
    if (entry->lru_prev != previous) {
      return false;
    }
    const auto it = visible_line_cache_.find(entry->key);
    if (it == visible_line_cache_.end() || &it->second != entry) {
      return false;
    }
    previous = entry;
    if (++forward > visible_line_cache_.size()) {
      return false;
    }
  }
  return forward == visible_line_cache_.size() && previous == visible_line_lru_.tail;
}

void TextLayoutCache::RelinkVisibleLineLruIfDetached() const {
  if (visible_line_lru_.head != nullptr || visible_line_cache_.empty()) {
    return;
  }
  for (auto& [key, entry] : visible_line_cache_) {
    entry.lru_prev = nullptr;
    entry.lru_next = nullptr;
    VisibleLineLruPushBack(entry);
  }
}

void TextLayoutCache::InvalidateVisibleLineCacheFrom(std::size_t start_line,
                                                     std::uint64_t content_revision) const {
  visible_line_cache_content_revision_ = content_revision;
  RelinkVisibleLineLruIfDetached();
  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (it->first.line_index >= start_line) {
      VisibleLineLruUnlink(it->second);
      const auto next = std::next(it);
      visible_line_node_pool_.Retire(visible_line_cache_.extract(it));
      it = next;
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
    bytes += sizeof(VisibleLineCacheKey) + sizeof(VisibleLineCacheEntry) + sizeof(void*) * 2;
    bytes += entry.layout.text.capacity();
    bytes += entry.layout.source_columns.capacity() * sizeof(std::size_t);
    bytes += entry.layout.text_offsets.capacity() * sizeof(std::size_t);
  }
  for (const auto& node : visible_line_node_pool_.nodes()) {
    const LayoutLine& layout = node.mapped().layout;
    bytes += sizeof(VisibleLineCacheKey) + sizeof(VisibleLineCacheEntry) + sizeof(void*) * 2;
    bytes += layout.text.capacity();
    bytes += layout.source_columns.capacity() * sizeof(std::size_t);
    bytes += layout.text_offsets.capacity() * sizeof(std::size_t);
  }
  return bytes;
}

void TextLayoutCache::InvalidateAll() {
  ClearVisibleLineAndMaxColumns();
  DropWrappedRowLayouts();
  plain_prefix_memo_ = {};
  visible_line_node_pool_.Release();
}

}
