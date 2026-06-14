#include "workspace/CompareMergeRender.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <system_error>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kCollapsedContextButtonGap = 6.0f;
constexpr float kCollapsedContextButtonHeight = 16.0f;
constexpr float kCollapsedContextButtonHorizontalPadding = 12.0f;

float CollapsedContextButtonWidth(const render::TextRenderer& text_renderer,
                                  std::string_view label) {
  return std::max(48.0f, text_renderer.MeasureWidth(label) + kCollapsedContextButtonHorizontalPadding);
}

}  // namespace

std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch) {
  const auto [end, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
  if (ec != std::errc{}) {
    return {};
  }
  return std::string_view(scratch.data(), static_cast<std::size_t>(end - scratch.data()));
}

CollapsedContextActionRects BuildCollapsedContextActionRects(
    const render::TextRenderer& text_renderer,
    const SDL_FRect& row_rect,
    bool show_previous,
    bool show_next) {
  const float center_y = row_rect.y + std::floor(std::max(0.0f, row_rect.h - kCollapsedContextButtonHeight) * 0.5f);
  const float all_width = CollapsedContextButtonWidth(text_renderer, "Show all");
  const float next_width = show_next ? CollapsedContextButtonWidth(text_renderer, "Show next 20") : 0.0f;
  const float previous_width =
      show_previous ? CollapsedContextButtonWidth(text_renderer, "Show previous 20") : 0.0f;

  float x = row_rect.x + row_rect.w - all_width;
  if (show_next) {
    x -= kCollapsedContextButtonGap + next_width;
  }
  if (show_previous) {
    x -= kCollapsedContextButtonGap + previous_width;
  }

  CollapsedContextActionRects rects;
  rects.text_right_edge = std::max(row_rect.x, x - 10.0f);
  if (show_previous) {
    rects.previous_rect = MakeRect(x, center_y, previous_width, kCollapsedContextButtonHeight);
    x += previous_width + kCollapsedContextButtonGap;
  }
  rects.all_rect = MakeRect(x, center_y, all_width, kCollapsedContextButtonHeight);
  x += all_width + kCollapsedContextButtonGap;
  if (show_next) {
    rects.next_rect = MakeRect(x, center_y, next_width, kCollapsedContextButtonHeight);
  }
  return rects;
}

SDL_FRect CompareCollapsedContextBlockRect(const SDL_FRect& editor_surface,
                                           float rows_y,
                                           float line_height,
                                           bool show_vertical_scrollbar,
                                           int visible_row) {
  const float right_reserved = show_vertical_scrollbar ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, editor_surface.w - right_reserved);
  const float row_y = rows_y + static_cast<float>(visible_row) * line_height - 1.0f;
  return MakeRect(editor_surface.x + 4.0f, row_y, std::max(0.0f, content_width - 8.0f), line_height);
}

}  // namespace microide::workspace
