#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "render/PaintElement.h"
#include "render/RendererInfo.h"

namespace microide::render {

// A line whose pixels do not depend on the clip rect it is drawn under.
//
// SDL's software renderer draws a line natively (it ignores
// SDL_HINT_RENDER_LINE_METHOD) and clips it BEFORE rasterizing: Bresenham then
// starts from the clipped endpoint, so a diagonal cut by a clip lands on other
// pixels than the same line drawn whole (SDL_drawline.c says so itself: "We
// don't actually want to clip, as it may change line slope"). A partial frame's
// clip through an icon -- the overflow chevron beside an open menu -- then drew
// it a pixel off from the full frame the redraw verifier compares it with (CI
// qa-asan, monkey seed 4: QA-STALE at the clip's left edge).
//
// On that path the line is rasterized here, with SDL's own algorithm (BLINE in
// SDL_draw.h, endpoints truncated as SW_QueueDrawLines does, the end pixel
// drawn), and drawn as points, which are clipped one by one: the same pixels as
// SDL's unclipped line, whatever the clip. Every other path -- a GPU renderer, a
// scaled or logical view, where SDL draws lines as geometry -- is already
// clip-stable and draws through SDL_RenderLine unchanged.
inline void RenderClipStableLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2) {
  NotePaint(renderer, SDL_FRect{std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1) + 1.0f,
                                std::abs(y2 - y1) + 1.0f});
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  if (RendererIsGpu(renderer) || !SDL_GetRenderScale(renderer, &scale_x, &scale_y) || scale_x != 1.0f ||
      scale_y != 1.0f || !SDL_GetRenderLogicalPresentation(renderer, nullptr, nullptr, &mode) ||
      mode != SDL_LOGICAL_PRESENTATION_DISABLED) {
    SDL_RenderLine(renderer, x1, y1, x2, y2);
    return;
  }
  const int ix1 = static_cast<int>(x1);
  const int iy1 = static_cast<int>(y1);
  const int ix2 = static_cast<int>(x2);
  const int iy2 = static_cast<int>(y2);
  const int dx = std::abs(ix2 - ix1);
  const int dy = std::abs(iy2 - iy1);
  const bool x_major = dx >= dy;
  const int count = (x_major ? dx : dy) + 1;
  int d = x_major ? 2 * dy - dx : 2 * dx - dy;
  const int dinc1 = x_major ? dy * 2 : dx * 2;
  const int dinc2 = x_major ? (dy - dx) * 2 : (dx - dy) * 2;
  int xinc1 = x_major ? 1 : 0;
  int xinc2 = 1;
  int yinc1 = x_major ? 0 : 1;
  int yinc2 = 1;
  if (ix1 > ix2) {
    xinc1 = -xinc1;
    xinc2 = -xinc2;
  }
  if (iy1 > iy2) {
    yinc1 = -yinc1;
    yinc2 = -yinc2;
  }
  // Icons are short strokes; a long rule goes in batches.
  std::array<SDL_FPoint, 64> batch{};
  std::size_t used = 0;
  int x = ix1;
  int y = iy1;
  for (int i = 0; i < count; ++i) {
    batch[used++] = SDL_FPoint{static_cast<float>(x), static_cast<float>(y)};
    if (used == batch.size()) {
      SDL_RenderPoints(renderer, batch.data(), static_cast<int>(used));
      used = 0;
    }
    if (d < 0) {
      d += dinc1;
      x += xinc1;
      y += yinc1;
    } else {
      d += dinc2;
      x += xinc2;
      y += yinc2;
    }
  }
  if (used > 0) {
    SDL_RenderPoints(renderer, batch.data(), static_cast<int>(used));
  }
}

}  // namespace microide::render
