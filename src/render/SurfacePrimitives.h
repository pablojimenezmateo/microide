#pragma once

#include "render/PaintElement.h"
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
  NotePaint(renderer, rect);
  SetDrawColor(renderer, color);
  SDL_RenderFillRect(renderer, &rect);
}

inline void OutlineRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  NotePaint(renderer, rect);
  SetDrawColor(renderer, color);
  SDL_RenderRect(renderer, &rect);
}

// The renderer's draw calls for paint that batches or sets its own draw colour
// (a row's coalesced fills, a glyph's geometry, an image), reported to the
// paint-overrun recorder as FillRect is: while it records, each draw's extent is
// checked against the element and the paint pass it lands in (PaintElement,
// PaintPassScope). Off, a report is one flag test. Paint draws through these or
// the primitives above, never SDL_Render* itself (CheckSdlDrawCallsStayInTheAllowlist).
inline void FillRectInDrawColor(SDL_Renderer* renderer, const SDL_FRect& rect) {
  NotePaint(renderer, rect);
  SDL_RenderFillRect(renderer, &rect);
}

inline void FillRectsInDrawColor(SDL_Renderer* renderer, const SDL_FRect* rects, int count) {
  if (PaintOverrunRecordingEnabled()) {
    for (int i = 0; i < count; ++i) {
      NotePaint(renderer, rects[i]);
    }
  }
  SDL_RenderFillRects(renderer, rects, count);
}

inline void OutlineRectInDrawColor(SDL_Renderer* renderer, const SDL_FRect& rect) {
  NotePaint(renderer, rect);
  SDL_RenderRect(renderer, &rect);
}

// The box a run of points spans: what a polyline or a geometry submission covers.
template <typename PointOf>
inline SDL_FRect BoundsOf(int count, PointOf&& point_of) {
  if (count <= 0) {
    return SDL_FRect{};
  }
  SDL_FPoint low = point_of(0);
  SDL_FPoint high = low;
  for (int i = 1; i < count; ++i) {
    const SDL_FPoint p = point_of(i);
    low = SDL_FPoint{std::min(low.x, p.x), std::min(low.y, p.y)};
    high = SDL_FPoint{std::max(high.x, p.x), std::max(high.y, p.y)};
  }
  return SDL_FRect{low.x, low.y, high.x - low.x + 1.0f, high.y - low.y + 1.0f};
}

inline void DrawPolyline(SDL_Renderer* renderer, const SDL_FPoint* points, int count) {
  if (PaintOverrunRecordingEnabled()) {
    NotePaint(renderer, BoundsOf(count, [points](int i) { return points[i]; }));
  }
  SDL_RenderLines(renderer, points, count);
}

inline void DrawGeometry(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Vertex* vertices, int vertex_count,
                         const int* indices, int index_count) {
  if (PaintOverrunRecordingEnabled()) {
    NotePaint(renderer, BoundsOf(vertex_count, [vertices](int i) { return vertices[i].position; }));
  }
  SDL_RenderGeometry(renderer, texture, vertices, vertex_count, indices, index_count);
}

inline void DrawTexture(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect* source,
                        const SDL_FRect& destination) {
  NotePaint(renderer, destination);
  SDL_RenderTexture(renderer, texture, source, &destination);
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
