#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <SDL3/SDL.h>

namespace microide::render {
struct Theme;
class TextRenderer;
}

namespace microide::workspace {

// Convert `value` to its decimal text representation in `scratch` and return a
// view into the populated bytes. Returns an empty view if std::to_chars fails.
std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch);


// Fill the scrollbar track rect with the theme's raised-surface color.
void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track);

// Fill the scrollbar thumb rect; `active` swaps the disabled-text fill for the
// accent color to indicate hover/drag.
void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false);

// Draw both the track and thumb in one call.
void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false);

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
