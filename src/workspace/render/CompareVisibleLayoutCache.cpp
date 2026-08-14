#include "workspace/render/CompareVisibleLayoutCache.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "workspace/git/CompareTabReview.h"

namespace microide::workspace {
namespace {

constexpr std::size_t kMaxCompareLayoutBytes = 1u << 20;  // 1 MiB
constexpr std::size_t kCompareVisibleLayoutCacheLimit = 512;

}  // namespace

void PrepareCompareVisibleLayoutsForWindow(CompareTabState& compare_tab,
                                           std::size_t visual_start_row,
                                           std::size_t visual_end_row,
                                           std::size_t left_visible_columns,
                                           std::size_t right_visible_columns) {
  if (compare_tab.visible_layout_cache_model_revision != compare_tab.model_revision) {
    compare_tab.visible_layout_cache_model_revision = compare_tab.model_revision;
    compare_tab.visible_layout_cache.clear();
    compare_tab.visible_layout_cache_index.clear();
  }

  const std::size_t tab_size = compare_tab.right_viewport.tab_size();
  const std::size_t clamped_end = std::min(visual_end_row, CompareTabVisualRowCount(compare_tab));
  if (visual_start_row >= clamped_end) {
    return;
  }

  const auto ensure_layout = [&](std::size_t model_row,
                                 bool right_side,
                                 std::size_t row_visual_start,
                                 std::size_t visible_columns,
                                 std::string_view text) {
    if (text.size() > kMaxCompareLayoutBytes || visible_columns == 0) {
      return;
    }
    const CompareVisibleLayoutCacheKey key{
        .model_row = model_row,
        .horizontal_scroll = row_visual_start,
        .visible_columns = visible_columns,
        .tab_size = tab_size,
        .right_side = right_side,
    };
    if (compare_tab.visible_layout_cache_index.find(key) !=
        compare_tab.visible_layout_cache_index.end()) {
      return;
    }
    if (compare_tab.visible_layout_cache.size() >= kCompareVisibleLayoutCacheLimit) {
      compare_tab.visible_layout_cache.clear();
      compare_tab.visible_layout_cache_index.clear();
    }
    CompareVisibleLayoutCacheEntry entry;
    entry.model_row = model_row;
    entry.horizontal_scroll = row_visual_start;
    entry.visible_columns = visible_columns;
    entry.tab_size = tab_size;
    entry.right_side = right_side;
    entry.layout = editor::TextLayout::BuildVisibleLine(text, row_visual_start,
                                                        visible_columns, tab_size);
    compare_tab.visible_layout_cache_index.emplace(key, compare_tab.visible_layout_cache.size());
    compare_tab.visible_layout_cache.push_back(std::move(entry));
  };

  const bool wrapped = compare_tab.wrap_layout.active();
  for (std::size_t visual_index = visual_start_row; visual_index < clamped_end; ++visual_index) {
    const DiffWrapRow wrap_row = compare_tab.wrap_layout.RowAt(visual_index);
    const compare::ComparePresentationRow* presentation_row =
        CompareTabPresentationRowAt(compare_tab, wrap_row.unit);
    if (presentation_row == nullptr ||
        presentation_row->kind != compare::ComparePresentationRowKind::Model ||
        presentation_row->model_row_index >= compare_tab.model.rows.size()) {
      continue;
    }
    const std::size_t model_row = presentation_row->model_row_index;
    const compare::CompareRow& row = compare_tab.model.rows[model_row];
    if (row.left_line > 0 && wrap_row.left_present) {
      const std::size_t start = wrapped ? wrap_row.left_start : compare_tab.horizontal_scroll;
      const std::size_t columns =
          wrapped ? wrap_row.left_end - wrap_row.left_start : left_visible_columns;
      ensure_layout(model_row, false, start, columns, row.left_text);
    }
    if (row.right_line > 0 && wrap_row.right_present) {
      const std::size_t start = wrapped ? wrap_row.right_start : compare_tab.horizontal_scroll;
      const std::size_t columns =
          wrapped ? wrap_row.right_end - wrap_row.right_start : right_visible_columns;
      ensure_layout(model_row, true, start, columns, row.right_text);
    }
  }
}

const editor::LayoutLine* CompareVisibleLayoutForRow(const CompareTabState& compare_tab,
                                                     std::size_t model_row,
                                                     bool right_side,
                                                     std::size_t row_visual_start,
                                                     std::size_t visible_columns) {
  if (compare_tab.visible_layout_cache_model_revision != compare_tab.model_revision) {
    return nullptr;
  }
  const CompareVisibleLayoutCacheKey key{
      .model_row = model_row,
      .horizontal_scroll = row_visual_start,
      .visible_columns = visible_columns,
      .tab_size = compare_tab.right_viewport.tab_size(),
      .right_side = right_side,
  };
  const auto it = compare_tab.visible_layout_cache_index.find(key);
  if (it == compare_tab.visible_layout_cache_index.end() ||
      it->second >= compare_tab.visible_layout_cache.size()) {
    return nullptr;
  }
  return &compare_tab.visible_layout_cache[it->second].layout;
}

}  // namespace microide::workspace
