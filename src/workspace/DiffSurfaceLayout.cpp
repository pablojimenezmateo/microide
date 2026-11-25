#include "workspace/DiffSurfaceLayout.h"

#include <algorithm>

#include "workspace/MergeToolbarMetrics.h"
#include "workspace/MergeWrapRows.h"
#include "workspace/git/CompareTabReview.h"

namespace microide::workspace {

ScrollSurfaceLayout ComputeCompareScrollLayout(const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                                               const CompareTabState& compare_tab) {
  const std::size_t content_columns = compare_tab.right_viewport.soft_wrap() ? 0 : compare_tab.max_visual_columns;
  const SDL_FRect rows_area =
      MakeRect(rect.x, surface.rows_y, rect.w, std::max(0.0f, rect.y + rect.h - surface.rows_y));
  return ComputeScrollSurfaceLayout(rows_area, CompareTabVisualRowCount(compare_tab), surface.visible_rows,
                                    compare_tab.scroll_row, content_columns, surface.visible_columns,
                                    compare_tab.horizontal_scroll);
}

int CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) {
  return std::max(0, static_cast<int>(CompareTabVisualRowCount(compare_tab)) - std::max(1, visible_rows));
}

std::size_t CompareMaxScrollColumn(const CompareTabState& compare_tab, std::size_t visible_columns) {
  if (compare_tab.right_viewport.soft_wrap() || compare_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return compare_tab.max_visual_columns - visible_columns;
}

void ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) {
  compare_tab.scroll_row = std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

void ClampCompareHorizontalScroll(CompareTabState& compare_tab, std::size_t visible_columns) {
  compare_tab.horizontal_scroll =
      std::min(compare_tab.horizontal_scroll, CompareMaxScrollColumn(compare_tab, visible_columns));
}

ScrollSurfaceLayout ComputeMergeScrollLayout(const SDL_FRect& rect, const MergeSurfaceLayout& surface,
                                             const MergeTabState& merge_tab) {
  const std::size_t content_columns = merge_tab.result_viewport.soft_wrap() ? 0 : merge_tab.max_visual_columns;
  const SDL_FRect rows_area =
      MakeRect(rect.x, surface.rows_y, rect.w, std::max(0.0f, rect.y + rect.h - surface.rows_y));
  return ComputeScrollSurfaceLayout(rows_area, MergeTotalVisualRowCount(merge_tab), surface.visible_rows,
                                    merge_tab.scroll_row, content_columns, surface.visible_columns,
                                    merge_tab.horizontal_scroll);
}

int MergeMaxScrollRow(const MergeTabState& merge_tab, int visible_rows) {
  return std::max(0, static_cast<int>(MergeTotalVisualRowCount(merge_tab)) - std::max(1, visible_rows));
}

std::size_t MergeMaxScrollColumn(const MergeTabState& merge_tab, std::size_t visible_columns) {
  if (merge_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return merge_tab.max_visual_columns - visible_columns;
}

void ClampMergeScrollRow(MergeTabState& merge_tab, int visible_rows) {
  merge_tab.scroll_row = std::clamp(merge_tab.scroll_row, 0, MergeMaxScrollRow(merge_tab, visible_rows));
}

void ClampMergeHorizontalScroll(MergeTabState& merge_tab, std::size_t visible_columns) {
  merge_tab.horizontal_scroll = std::min(merge_tab.horizontal_scroll, MergeMaxScrollColumn(merge_tab, visible_columns));
}

void RevealCompareSelectedRow(CompareTabState& compare_tab, int visible_rows) {
  const int rows = std::max(1, visible_rows);
  const int first = static_cast<int>(ComparePresentationRowToVisualRow(compare_tab, compare_tab.selected_row));
  const int span = static_cast<int>(compare_tab.wrap_layout.RowSpanForUnit(compare_tab.selected_row));
  const int last = first + std::max(1, span) - 1;
  if (first < compare_tab.scroll_row) {
    compare_tab.scroll_row = first;
  } else if (last >= compare_tab.scroll_row + rows) {
    compare_tab.scroll_row = last - rows + 1;
  }
  ClampCompareScrollRow(compare_tab, visible_rows);
}

std::array<SDL_FRect, 4> BuildMergeResultActionButtonRects(const MergeSurfaceLayout& surface,
                                                           const MergeInteractionLayout& interaction,
                                                           const MergeTrackedConflict& conflict) {
  return ComputeMergeResultActionButtonRects(
      surface.center_x + surface.gutter_width, surface.rows_y, interaction.content_bottom,
      ComputeVisibleLineRangeRect(interaction.result.rect, interaction.result.lines, conflict.start_line,
                                  std::max(conflict.end_line, conflict.start_line + std::size_t{1})),
      interaction.result_action_widths, kMergeToolbarButtonHeight, kMergeToolbarButtonGap,
      surface.center_x + surface.center_width);
}
}
