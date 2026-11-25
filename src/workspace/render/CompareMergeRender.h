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

std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch);

float CompareButtonWidth(const render::TextRenderer& text_renderer, std::string_view label);
SDL_FRect CompareButtonInRow(float x, const SDL_FRect& row_rect, float width);
inline constexpr float kCompareButtonGap = 6.0f;

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

SDL_FRect CompareCollapsedContextBlockRect(const SDL_FRect& editor_surface,
                                           float rows_y,
                                           float line_height,
                                           bool show_vertical_scrollbar,
                                           int visible_row);

}
