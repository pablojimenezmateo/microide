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

struct InlineLineSplice {
  std::string_view inserted_text;
  bool valid = false;
};

class TextLayoutCache {
 public:
  struct WrappedRow {
    std::size_t line_index = 0;
    std::size_t visual_start = 0;
    std::size_t visual_end = 0;
    std::size_t indent = 0;
  };

  struct Stats {
    std::size_t visible_line_queries = 0;
    std::size_t visible_line_hits = 0;
    std::size_t visible_line_evictions = 0;
  };

  const LayoutLine& VisibleLineLayoutRefCached(LineSpan lines,
                                               std::size_t line_index,
                                               std::size_t horizontal_scroll,
                                               std::size_t visible_columns,
                                               std::size_t tab_size,
                                               std::uint64_t content_revision,
                                               LineLayoutFacts facts_hint = {}) const;

  void EnsureWrappedRowLayouts(LineSpan lines,
                               std::size_t tab_size,
                               std::size_t visible_columns,
                               bool soft_wrap,
                               const FoldingModel* folding_model,
                               std::uint64_t layout_shape_revision,
                               std::uint64_t content_revision) const;

  static constexpr std::size_t kNoEditColumn = static_cast<std::size_t>(-1);
  bool UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                  std::size_t removed_count,
                                  std::size_t inserted_count,
                                  LineSpan lines,
                                  std::size_t tab_size,
                                  std::size_t visible_columns,
                                  bool soft_wrap,
                                  const FoldingModel* folding_model,
                                  std::uint64_t layout_shape_revision,
                                  std::uint64_t content_revision,
                                  std::size_t edit_column = kNoEditColumn) const;

  WrappedRow WrappedRowAt(std::size_t visual_row_index,
                          std::size_t horizontal_scroll,
                          std::size_t visible_columns) const;
  std::size_t WrappedRowCount(std::size_t lines_size) const;
  std::size_t WrappedLineRowOffset(std::size_t line_index) const;
  std::pair<std::size_t, std::size_t> WrappedRowRangeForLine(std::size_t line_index,
                                                             std::size_t lines_size) const;
  std::size_t WrappedRowForVisualColumn(std::size_t first_row,
                                        std::size_t last_row,
                                        std::size_t visual_column,
                                        bool prefer_previous_row) const;
  bool wrapped_row_layouts_trivial() const { return wrapped_row_layouts_trivial_; }

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
  void UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                        std::size_t removed_count,
                                        std::size_t inserted_count,
                                        LineSpan lines,
                                        std::size_t tab_size,
                                        std::uint64_t content_revision,
                                        InlineLineSplice splice = {});

  void InvalidateAll();
  void ParkLineWidthsForTabSizeChange();
  void ArmLineWidthParking() { line_width_parking_armed_ = true; }
  void ClearVisibleLineAndMaxColumns();
  void DropWrappedRowLayouts();
  void InvalidateVisibleLineCacheFrom(std::size_t start_line, std::uint64_t content_revision) const;

  bool has_wrapped_line_row_offsets(std::size_t lines_size) const;

  Stats stats() const {
    return Stats{visible_line_queries_, visible_line_hits_, visible_line_evictions_};
  }
  void ResetStats() const {
    visible_line_queries_ = 0;
    visible_line_hits_ = 0;
    visible_line_evictions_ = 0;
  }

  std::size_t ApproximateResidentBytes() const;

  bool VisibleLineLruIsConsistentForTesting() const;

  std::string VerifyAgainstFreshBuildForTesting(LineSpan lines,
                                                std::size_t tab_size,
                                                std::size_t visible_columns,
                                                bool soft_wrap,
                                                const FoldingModel* folding_model,
                                                std::uint64_t layout_shape_revision,
                                                std::uint64_t content_revision) const;

#ifndef NDEBUG
  std::size_t wrapped_row_layout_build_count_for_debug() const {
    return wrapped_row_layout_build_count_;
  }
  std::size_t wrapped_row_incremental_inplace_count_for_debug() const {
    return wrapped_row_incremental_inplace_count_;
  }
  std::size_t wrapped_row_incremental_splice_count_for_debug() const {
    return wrapped_row_incremental_splice_count_;
  }
  std::size_t visual_column_incremental_inplace_count_for_debug() const {
    return visual_column_incremental_inplace_count_;
  }
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

  struct PackedLineWidth {
    static constexpr std::size_t kPlainAsciiBit = std::size_t{1}
                                                  << (8 * sizeof(std::size_t) - 1);
    static constexpr std::size_t kTabIndentedBit = std::size_t{1}
                                                   << (8 * sizeof(std::size_t) - 2);
    static constexpr std::size_t kLeadingTabsShift = 48;
    static constexpr std::size_t kLeadingTabsMax = 255;
    static constexpr std::size_t kWidthMask = (std::size_t{1} << kLeadingTabsShift) - 1;
    std::size_t packed = 0;

    static PackedLineWidth From(LineLayoutFacts facts) {
      std::size_t packed = facts.visual_columns & kWidthMask;
      if (facts.plain_ascii) {
        packed |= kPlainAsciiBit;
      } else if (facts.tab_indented && facts.leading_tabs <= kLeadingTabsMax) {
        packed |= kTabIndentedBit | (facts.leading_tabs << kLeadingTabsShift);
      }
      return PackedLineWidth{packed};
    }
    std::size_t visual_columns() const { return packed & kWidthMask; }
    LineLayoutFacts facts() const {
      const bool tab_indented = (packed & kTabIndentedBit) != 0;
      return LineLayoutFacts{
          .visual_columns = visual_columns(),
          .plain_ascii = (packed & kPlainAsciiBit) != 0,
          .known = true,
          .tab_indented = tab_indented,
          .leading_tabs = tab_indented ? (packed >> kLeadingTabsShift) & kLeadingTabsMax : 0,
      };
    }
  };

  static bool DescribesTheTextJustBefore(std::uint64_t table_revision,
                                         std::uint64_t content_revision) {
    return content_revision != 0 && table_revision == content_revision - 1;
  }
  bool LineWidthsAreCurrent(std::size_t lines_size,
                            std::size_t tab_size,
                            std::uint64_t content_revision) const {
    return cached_max_visual_columns_tab_size_ == tab_size &&
           cached_max_visual_columns_content_revision_ == content_revision &&
           cached_visual_line_columns_.size() == lines_size;
  }

#ifndef NDEBUG
  void VerifyLineWidthTableIfRequested(LineSpan lines,
                                       std::size_t tab_size,
                                       std::uint64_t content_revision) const;
#endif

  static void WrapSingleLine(std::string_view line_text,
                             std::size_t line_index,
                             std::size_t tab_size,
                             std::size_t wrap_columns,
                             std::vector<WrappedRow>& out);

  static constexpr std::size_t kVisibleLineCacheLimit = 256;

  struct VisibleLineCacheEntry {
    LayoutLine layout;
    VisibleLineCacheKey key;
    VisibleLineCacheEntry* lru_prev = nullptr;
    VisibleLineCacheEntry* lru_next = nullptr;
  };

  using VisibleLineCacheMap =
      std::unordered_map<VisibleLineCacheKey, VisibleLineCacheEntry, VisibleLineCacheKeyHash>;

  class VisibleLineNodePool {
   public:
    VisibleLineNodePool() = default;
    VisibleLineNodePool(const VisibleLineNodePool&) {}
    VisibleLineNodePool& operator=(const VisibleLineNodePool&) {
      nodes_.clear();
      return *this;
    }
    VisibleLineNodePool(VisibleLineNodePool&&) = default;
    VisibleLineNodePool& operator=(VisibleLineNodePool&&) = default;

    bool empty() const { return nodes_.empty(); }
    std::size_t size() const { return nodes_.size(); }
    const std::vector<VisibleLineCacheMap::node_type>& nodes() const { return nodes_; }

    void Retire(VisibleLineCacheMap::node_type node) {
      if (node.empty() || nodes_.size() >= kVisibleLineCacheLimit) {
        return;
      }
      node.mapped().lru_prev = nullptr;
      node.mapped().lru_next = nullptr;
      nodes_.push_back(std::move(node));
    }

    VisibleLineCacheMap::node_type Take() {
      VisibleLineCacheMap::node_type node = std::move(nodes_.back());
      nodes_.pop_back();
      return node;
    }

    void Release() {
      nodes_.clear();
      nodes_.shrink_to_fit();
    }

   private:
    std::vector<VisibleLineCacheMap::node_type> nodes_;
  };

  struct VisibleLineLruEnds {
    VisibleLineCacheEntry* head = nullptr;
    VisibleLineCacheEntry* tail = nullptr;

    VisibleLineLruEnds() = default;
    VisibleLineLruEnds(const VisibleLineLruEnds&) {}
    VisibleLineLruEnds& operator=(const VisibleLineLruEnds&) {
      head = nullptr;
      tail = nullptr;
      return *this;
    }
    VisibleLineLruEnds(VisibleLineLruEnds&&) = default;
    VisibleLineLruEnds& operator=(VisibleLineLruEnds&&) = default;
  };

  void RetireVisibleLineNodes();
  void RelinkVisibleLineLruIfDetached() const;
  void VisibleLineLruUnlink(VisibleLineCacheEntry& entry) const;
  void VisibleLineLruPushBack(VisibleLineCacheEntry& entry) const;
  void VisibleLineLruTouch(VisibleLineCacheEntry& entry) const {
    if (visible_line_lru_.tail == &entry) {
      return;
    }
    VisibleLineLruUnlink(entry);
    VisibleLineLruPushBack(entry);
  }

  mutable VisibleLineCacheMap visible_line_cache_;
  mutable std::uint64_t visible_line_cache_content_revision_ = 0;
  mutable VisibleLineNodePool visible_line_node_pool_;
  mutable VisibleLineLruEnds visible_line_lru_;
  mutable std::size_t visible_line_queries_ = 0;
  mutable std::size_t visible_line_hits_ = 0;
  mutable std::size_t visible_line_evictions_ = 0;
  mutable std::string line_window_scratch_;

 public:
  std::size_t PlainAsciiPrefixEnd(LineSpan lines,
                                  std::size_t line_index,
                                  std::size_t probe,
                                  std::uint64_t content_revision) const;

  bool PlainAsciiThroughColumn(LineSpan lines,
                               std::size_t line_index,
                               std::size_t column,
                               std::uint64_t content_revision) const;
  void CarryPlainPrefixMemosAcrossEdit(LineSpan lines,
                                       std::size_t start_line,
                                       std::size_t edit_column,
                                       std::uint64_t old_revision,
                                       std::uint64_t new_revision) const;

 private:
  struct PlainPrefixMemo {
    std::size_t line_index = 0;
    std::size_t line_length = 0;
    std::uint64_t content_revision = 0;
    std::size_t scanned_through = 0;
    std::size_t first_non_plain = 0;
    bool valid = false;
  };
#ifndef NDEBUG
  void VerifyWrappedRowsAgainstFullRewrap(LineSpan lines,
                                          std::size_t line_index,
                                          std::size_t tab_size,
                                          std::size_t wrap_columns) const;
#endif
  std::size_t PlainAsciiPrefixEndInto(LineSpan lines,
                                      std::size_t line_index,
                                      std::size_t probe,
                                      std::uint64_t content_revision,
                                      std::string& scratch) const;
  static constexpr std::size_t kPlainPrefixMemoSlots = 4;
  mutable std::array<PlainPrefixMemo, kPlainPrefixMemoSlots> plain_prefix_memo_{};
  mutable std::size_t plain_prefix_memo_next_ = 0;
  mutable std::string column_scan_scratch_;
  mutable std::string wrapped_row_tail_scratch_;

  mutable std::vector<WrappedRow> wrapped_row_layouts_;
  mutable std::vector<std::size_t> wrapped_line_row_offsets_;
  mutable std::size_t wrapped_row_layouts_tab_size_ = 0;
  mutable std::size_t wrapped_row_layouts_visible_columns_ = 0;
  mutable std::uint64_t wrapped_row_layouts_layout_shape_revision_ = 0;
  mutable bool wrapped_row_layouts_soft_wrap_ = false;
  mutable const FoldingModel* wrapped_row_layouts_folding_model_ = nullptr;
  mutable std::size_t wrapped_row_layouts_fold_revision_ = 0;
  mutable std::uint64_t wrapped_row_layouts_content_revision_ = 0;
  mutable bool wrapped_row_layouts_trivial_ = false;
  mutable bool wrapped_row_layouts_fold_no_wrap_ = false;
  mutable std::vector<WrappedRow> wrapped_row_edit_scratch_;
  mutable std::vector<std::size_t> wrapped_row_edit_offsets_scratch_;
  mutable std::vector<std::size_t> wrapped_line_row_offsets_scratch_;
#ifndef NDEBUG
  mutable std::size_t wrapped_row_layout_build_count_ = 0;
  mutable std::size_t wrapped_row_incremental_inplace_count_ = 0;
  mutable std::size_t wrapped_row_incremental_splice_count_ = 0;
  mutable std::size_t visual_column_incremental_inplace_count_ = 0;
  mutable std::size_t visual_column_splice_derived_count_ = 0;
#endif

  mutable std::optional<std::size_t> cached_max_visual_columns_;
  mutable std::optional<std::size_t> cached_max_visual_columns_line_index_;
  mutable std::deque<PackedLineWidth> cached_visual_line_columns_;
  mutable std::vector<PackedLineWidth> inserted_columns_scratch_;
  mutable std::size_t cached_max_visual_columns_tab_size_ = 0;
  mutable std::uint64_t cached_max_visual_columns_content_revision_ = 0;
  struct ParkedLineWidths {
    std::optional<std::deque<PackedLineWidth>> widths;
    std::size_t tab_size = 0;
    std::uint64_t content_revision = 0;
    void Reset() {
      widths.reset();
      tab_size = 0;
      content_revision = 0;
    }
  };
  mutable ParkedLineWidths parked_line_widths_;
  mutable bool line_width_parking_armed_ = false;

  struct VisualColumnBlockMax {
    std::uint32_t columns = 0;
    std::uint32_t line = 0;
  };
  static constexpr std::size_t kVisualColumnBlockLines = 512;
  mutable std::vector<VisualColumnBlockMax> visual_column_block_max_;

  void RebuildVisualColumnBlockMaxima() const;
  void RefreshVisualColumnBlocksFor(std::size_t first_line, std::size_t last_line) const;
  std::size_t VisualColumnBlockCount() const {
    return (cached_visual_line_columns_.size() + kVisualColumnBlockLines - 1) /
           kVisualColumnBlockLines;
  }
};

}
