#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <SDL3/SDL.h>

#include "compare/CompareModel.h"
#include "editor/DecoratedTextGridRenderer.h"

namespace microide::render {
struct Theme;
class TextRenderer;
}

namespace microide::workspace {

// Convert `value` to its decimal text representation in `scratch` and return a
// view into the populated bytes. Returns an empty view if std::to_chars fails.
std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch);

// A compare surface's small text button (collapsed-context, header, review): its
// width for `label`, and its rect at `x` centred in `row_rect`, as tall as the row
// allows -- at a small editor font the row is shorter than a button, which hung
// below it (soak 8cffab44c08e).
float CompareButtonWidth(const render::TextRenderer& text_renderer, std::string_view label);
SDL_FRect CompareButtonInRow(float x, const SDL_FRect& row_rect, float width);
inline constexpr float kCompareButtonGap = 6.0f;

// Lays buttons out right to left from `right`, never past `left_limit`: Place
// returns a button's rect, or nullopt once one no longer fits.
struct CompareButtonRowPlacer {
  const render::TextRenderer& text_renderer;
  SDL_FRect row_rect{};
  float right = 0.0f;
  float left_limit = 0.0f;

  std::optional<SDL_FRect> Place(std::string_view label);
};

struct CollapsedContextActionRects {
  std::optional<SDL_FRect> previous_rect;
  SDL_FRect all_rect{};
  std::optional<SDL_FRect> next_rect;
  float text_right_edge = 0.0f;
};

CollapsedContextActionRects BuildCollapsedContextActionRects(
    const render::TextRenderer& text_renderer,
    const SDL_FRect& row_rect,
    bool show_previous,
    bool show_next);

// The on-screen rect of a collapsed-context summary block for the diff row at
// `visible_row` (its 0-based offset from the first painted row, not its presentation
// index). This is the exact rectangle the renderer fills and lays the action buttons
// out within — it excludes the vertical-scrollbar reserve and applies the 4px block
// inset. Every hit-test path must derive the action-button rects from this same rect,
// or the clickable region drifts right of the painted buttons (by the scrollbar
// reserve plus the inset). Feed the result to BuildCollapsedContextActionRects.
SDL_FRect CompareCollapsedContextBlockRect(const SDL_FRect& editor_surface,
                                           float rows_y,
                                           float line_height,
                                           bool show_vertical_scrollbar,
                                           int visible_row);

}  // namespace microide::workspace
