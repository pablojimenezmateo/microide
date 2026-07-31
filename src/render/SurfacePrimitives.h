#pragma once

#include "render/Theme.h"

#include <algorithm>

namespace microide::render {

enum class CardStyle {
  Raised,
  Overlay,
  Tooltip,
};

// Arms the renderer for a draw in `color`, picking the blend mode from its alpha.
//
// SDL's draw blend mode is ambient renderer state, so a translucent fill issued
// without arming it composites or overwrites depending on whatever drew last.
// That is not a cosmetic difference here: the scene is painted into a retained
// RGBA render target, so an overwriting translucent fill also stamps its own
// alpha into the target, and the region then re-composites against the window on
// every present until it collapses to the fill colour (TD-2026-07-30-100 — the
// modal backdrop eating the editor behind it). Every shell draw primitive routes
// through here so no surface has to remember. Opaque colours stay on
// SDL_BLENDMODE_NONE, which is both the faster path and what keeps the retained
// target's alpha at 255.
inline void SetDrawColor(SDL_Renderer* renderer, SDL_Color color) {
  SDL_SetRenderDrawBlendMode(
      renderer, color.a == SDL_ALPHA_OPAQUE ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

inline void FillRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SetDrawColor(renderer, color);
  SDL_RenderFillRect(renderer, &rect);
}

inline void OutlineRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  SetDrawColor(renderer, color);
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
