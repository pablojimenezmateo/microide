#pragma once

#include "render/Theme.h"

#include <algorithm>

namespace microide::render {

enum class CardStyle {
  Raised,
  Overlay,
  Tooltip,
};

inline void FillRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

inline void OutlineRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

inline SDL_Color CardBackground(const Theme& theme, CardStyle style) {
  switch (style) {
    case CardStyle::Overlay:
      return theme.overlay_background;
    case CardStyle::Tooltip:
    case CardStyle::Raised:
    default:
      return theme.surface_raised;
  }
}

inline void DrawCardFrame(SDL_Renderer* renderer,
                          const Theme& theme,
                          const SDL_FRect& rect,
                          CardStyle style) {
  FillRect(renderer, rect, CardBackground(theme, style));
  OutlineRect(renderer, rect, theme.border);
}

inline SDL_FRect DrawTitledCardFrame(SDL_Renderer* renderer,
                                     const Theme& theme,
                                     const SDL_FRect& rect,
                                     float header_height,
                                     CardStyle style) {
  DrawCardFrame(renderer, theme, rect, style);
  const SDL_FRect header = SDL_FRect{rect.x, rect.y, rect.w, std::max(0.0f, header_height)};
  FillRect(renderer, header, theme.chrome_background);
  FillRect(renderer,
           SDL_FRect{header.x, header.y + header.h - 1.0f, header.w, 1.0f},
           theme.border);
  return header;
}

}  // namespace microide::render
