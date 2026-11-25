#include "workspace/render/CompareMergeRender.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <charconv>
#include <system_error>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kCompareButtonHeight = 16.0f;
constexpr float kCompareButtonHorizontalPadding = 12.0f;

}

float CompareButtonWidth(const render::TextRenderer& text_renderer, std::string_view label) {
  return std::max(48.0f, text_renderer.MeasureWidth(label) + kCompareButtonHorizontalPadding);
}

SDL_FRect CompareButtonInRow(float x, const SDL_FRect& row_rect, float width) {
  const float height = std::min(kCompareButtonHeight, std::max(0.0f, row_rect.h));
  return MakeRect(x, row_rect.y + std::floor((row_rect.h - height) * 0.5f), width, height);
}

std::optional<SDL_FRect> CompareButtonRowPlacer::Place(std::string_view label) {
  const float width = CompareButtonWidth(text_renderer, label);
  if (right - width < left_limit) {
    return std::nullopt;
  }
  right -= width;
  const SDL_FRect rect = CompareButtonInRow(right, row_rect, width);
  right -= kCompareButtonGap;
  return rect;
}

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
  CompareButtonRowPlacer placer{.text_renderer = text_renderer,
                                .row_rect = row_rect,
                                .right = row_rect.x + row_rect.w,
                                .left_limit = -std::numeric_limits<float>::infinity()};
  CollapsedContextActionRects rects;
  if (show_next) {
    rects.next_rect = placer.Place("Show next 20");
  }
  rects.all_rect = *placer.Place("Show all");
  if (show_previous) {
    rects.previous_rect = placer.Place("Show previous 20");
  }
  rects.text_right_edge = std::max(row_rect.x, placer.right + kCompareButtonGap - 10.0f);
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

}
