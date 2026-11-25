#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

#include "editor/EditorViewRenderer.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

struct CompareSurfaceLayout {
  float line_height = 14.0f;
  float gutter_width = 28.0f;
  float divider_width = 18.0f;
  float left_width = 0.0f;
  float right_width = 0.0f;
  float left_x = 0.0f;
  float center_x = 0.0f;
  float right_x = 0.0f;
  float review_summary_y = 0.0f;
  float header_y = 0.0f;
  float rows_y = 0.0f;
  int visible_rows = 1;
  std::size_t left_visible_columns = 1;
  std::size_t right_visible_columns = 1;
  std::size_t visible_columns = 1;
  float min_divider_fraction = 0.5f;
  bool show_vertical = false;
  bool show_horizontal = false;
};

struct MergeSurfaceLayout {
  float line_height = 14.0f;
  float gutter_width = 28.0f;
  float divider_width = 16.0f;
  float left_width = 0.0f;
  float center_width = 0.0f;
  float right_width = 0.0f;
  float left_x = 0.0f;
  float center_x = 0.0f;
  float right_x = 0.0f;
  float button_y = 0.0f;
  float secondary_button_y = 0.0f;
  float header_y = 0.0f;
  float rows_y = 0.0f;
  int visible_rows = 1;
  std::size_t visible_columns = 1;
  float min_divider_fraction = 1.0f / 3.0f;
  bool show_vertical = false;
  bool show_horizontal = false;
};

struct MergeToolbarLayout {
  SDL_FRect prev_rect{};
  SDL_FRect next_rect{};
  SDL_FRect open_rect{};
  SDL_FRect save_rect{};
};

struct MergeResultInteractionLayout {
  SDL_FRect rect{};
  editor::EditorViewMetrics metrics{};
  VisibleLineRangeLayout lines{};
  TextGridInteractionLayout text{};
};

struct MergeInteractionLayout {
  float content_bottom = 0.0f;
  MergeResultInteractionLayout result{};
  float incoming_accept_button_width = 0.0f;
  float current_accept_button_width = 0.0f;
  std::array<float, 4> result_action_widths{};
};

int CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows);
std::size_t CompareMaxScrollColumn(const CompareTabState& compare_tab, std::size_t visible_columns);
void ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows);
void ClampCompareHorizontalScroll(CompareTabState& compare_tab, std::size_t visible_columns);
int MergeMaxScrollRow(const MergeTabState& merge_tab, int visible_rows);
std::size_t MergeMaxScrollColumn(const MergeTabState& merge_tab, std::size_t visible_columns);
void ClampMergeScrollRow(MergeTabState& merge_tab, int visible_rows);
void ClampMergeHorizontalScroll(MergeTabState& merge_tab, std::size_t visible_columns);

ScrollSurfaceLayout ComputeCompareScrollLayout(const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                                               const CompareTabState& compare_tab);
ScrollSurfaceLayout ComputeMergeScrollLayout(const SDL_FRect& rect, const MergeSurfaceLayout& surface,
                                             const MergeTabState& merge_tab);

void RevealCompareSelectedRow(CompareTabState& compare_tab, int visible_rows);
std::array<SDL_FRect, 4> BuildMergeResultActionButtonRects(const MergeSurfaceLayout& surface,
                                                           const MergeInteractionLayout& interaction,
                                                           const MergeTrackedConflict& conflict);

}
