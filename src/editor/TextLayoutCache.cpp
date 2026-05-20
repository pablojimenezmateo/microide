#include "editor/TextLayoutCache.h"

#include <algorithm>

#include "editor/FoldingModel.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::editor {

LayoutLine TextLayoutCache::VisibleLineLayoutCached(const std::vector<std::string>& lines,
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
  visible_line_cache_.emplace(cache_key, layout);
  visible_line_cache_order_.push_back(cache_key);
  return layout;
}

void TextLayoutCache::EnsureWrappedRowLayouts(const std::vector<std::string>& lines,
                                              std::size_t tab_size,
                                              std::size_t visible_columns,
                                              std::size_t horizontal_scroll,
                                              bool soft_wrap,
                                              const FoldingModel* folding_model,
                                              std::uint64_t layout_shape_revision) const {
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
    util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits, 0);
    wrapped_row_layouts_tab_size_ = tab_size;
    wrapped_row_layouts_visible_columns_ = visible_columns;
    wrapped_row_layouts_layout_shape_revision_ = layout_shape_revision;
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
  wrapped_row_layouts_.reserve(lines.size());
  wrapped_line_row_offsets_.reserve(lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits,
                              lines.size());
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns);
  std::size_t last_visible_row = 0;
  if (!soft_wrap) {
    // Non-soft-wrap with collapsed folds: still need wrapped_line_row_offsets_
    // so we can skip hidden lines, but the row payload is uniform.
    const WrappedRow row_template{0, horizontal_scroll, horizontal_scroll + visible_columns};
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      WrappedRow row = row_template;
      row.line_index = line_index;
      wrapped_row_layouts_.push_back(row);
    }
  } else {
    const std::size_t safe_tab_size = std::max<std::size_t>(1, tab_size);
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();

      const std::string& line_text = lines[line_index];
      if (line_text.empty()) {
        wrapped_row_layouts_.push_back(WrappedRow{line_index, 0, 0});
        continue;
      }

      // Single pass: walk the line tracking the visual column, the last whitespace
      // break opportunity, and the current row's text start. Break before any
      // character that would push the row past `wrap_columns`; prefer breaking
      // after the most recent whitespace if one is available inside the current
      // row. Hard-break inside a long word only when no whitespace fits.
      std::size_t row_start_visual = 0;
      std::size_t row_start_text = 0;
      std::size_t last_break_visual = 0;
      std::size_t last_break_text = 0;
      std::size_t visual = 0;
      std::size_t i = 0;
      const std::size_t line_size = line_text.size();
      while (i < line_size) {
        const unsigned char ch = static_cast<unsigned char>(line_text[i]);
        const std::size_t seq_len = util::Utf8SequenceLength(line_text, i);
        std::size_t next_visual;
        if (ch == '\t') {
          const std::size_t remainder = visual % safe_tab_size;
          next_visual =
              visual + (remainder == 0 ? safe_tab_size : safe_tab_size - remainder);
        } else {
          next_visual = visual + 1;
        }

        if (next_visual - row_start_visual > wrap_columns && i > row_start_text) {
          std::size_t break_visual;
          std::size_t break_text;
          if (last_break_text > row_start_text) {
            break_visual = last_break_visual;
            break_text = last_break_text;
          } else {
            break_visual = visual;
            break_text = i;
          }
          wrapped_row_layouts_.push_back(WrappedRow{line_index, row_start_visual, break_visual});
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
      wrapped_row_layouts_.push_back(WrappedRow{line_index, row_start_visual, visual});
    }
  }
  if (wrapped_row_layouts_.empty()) {
    wrapped_row_layouts_.push_back(WrappedRow{0, 0, 0});
    wrapped_line_row_offsets_.assign(lines.size(), 0);
  }
  wrapped_row_layouts_tab_size_ = tab_size;
  wrapped_row_layouts_visible_columns_ = visible_columns;
  wrapped_row_layouts_layout_shape_revision_ = layout_shape_revision;
  wrapped_row_layouts_soft_wrap_ = soft_wrap;
  wrapped_row_layouts_folding_model_ = folding_model;
  wrapped_row_layouts_fold_revision_ =
      folding_model != nullptr ? folding_model->revision() : 0;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
}

TextLayoutCache::WrappedRow TextLayoutCache::WrappedRowAt(std::size_t visual_row_index,
                                                          std::size_t /*lines_size*/,
                                                          std::size_t horizontal_scroll,
                                                          std::size_t visible_columns) const {
  if (wrapped_row_layouts_trivial_) {
    return WrappedRow{visual_row_index, horizontal_scroll, horizontal_scroll + visible_columns};
  }
  if (wrapped_row_layouts_.empty()) {
    return WrappedRow{};
  }
  const std::size_t clamped =
      std::min<std::size_t>(visual_row_index, wrapped_row_layouts_.size() - 1);
  return wrapped_row_layouts_[clamped];
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

std::size_t TextLayoutCache::MaxVisualColumns(const std::vector<std::string>& lines,
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
    const std::vector<std::string>& inserted_lines, const std::vector<std::string>& lines,
    std::size_t tab_size, std::uint64_t content_revision) {
  if (cached_max_visual_columns_tab_size_ != tab_size ||
      cached_visual_line_columns_.size() != lines.size() - inserted_lines.size() + removed_count) {
    InvalidateAll();
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
  cached_visual_line_columns_.erase(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(erase_end));
  cached_visual_line_columns_.insert(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      inserted_columns.begin(), inserted_columns.end());

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
  wrapped_row_layouts_trivial_ = false;
}

}  // namespace microide::editor
