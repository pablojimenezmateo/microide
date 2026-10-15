#pragma once

// Retained-scene redraw for pixel tests.
//
// The app loop does not repaint the window per event: it repaints only the rects
// an event's invalidation names, over the previous frame's pixels. A handler that
// forgets a rect therefore leaves stale pixels on screen, and nothing fails --
// unless a test paints the same way and compares the result with a full repaint
// of the same state. That is what this is for. Its pieces (the clip inflation,
// the pixel diff) lived as private copies in three test files; the diff is now
// the app's own (render/PixelDiff), shared with the QA redraw verifier.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "TestSupport.h"
#include "render/PixelDiff.h"
#include "render/TextRendererBackend.h"
#include "support/SoftwareCanvas.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests {

// The clip the app loop sets for one damage rect: the rect grown by the
// renderer's partial-redraw padding (glyph overhang), clamped to the canvas.
inline SDL_Rect InflateClipRect(const SDL_FRect& rect,
                                render::TextClipPadding padding,
                                int width,
                                int height) {
  const int x0 = std::max(0, static_cast<int>(std::floor(rect.x - padding.left)));
  const int y0 = std::max(0, static_cast<int>(std::floor(rect.y - padding.top)));
  const int x1 = std::min(width, static_cast<int>(std::ceil(rect.x + rect.w + padding.right)));
  const int y1 = std::min(height, static_cast<int>(std::ceil(rect.y + rect.h + padding.bottom)));
  return SDL_Rect{
      .x = x0,
      .y = y0,
      .w = std::max(0, x1 - x0),
      .h = std::max(0, y1 - y0),
  };
}

// The same comparison the app's QA redraw verifier runs (render/PixelDiff), over
// two surfaces of one 4-byte format -- every canvas here is RGBA8888.
inline render::PixelDiffResult DiffSurfaces(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr, "pixel comparisons should receive readable surfaces");
  Expect(lhs->format == rhs->format && SDL_BYTESPERPIXEL(lhs->format) == 4,
         "pixel comparisons should use canvases of one 4-byte pixel format");
  Expect(!SDL_MUSTLOCK(lhs) && !SDL_MUSTLOCK(rhs),
         "pixel comparisons should read surfaces that need no lock");
  const render::PixelDiffResult diff = render::DiffPixels(
      render::PixelImageView{static_cast<const std::uint8_t*>(lhs->pixels), lhs->w, lhs->h,
                             lhs->pitch},
      render::PixelImageView{static_cast<const std::uint8_t*>(rhs->pixels), rhs->w, rhs->h,
                             rhs->pitch});
  Expect(!diff.size_mismatch, "pixel comparisons should use canvases with matching dimensions");
  return diff;
}

inline std::size_t CountPixelDifferences(SDL_Surface* lhs, SDL_Surface* rhs) {
  return DiffSurfaces(lhs, rhs).count;
}

inline std::size_t CountPixelDifferences(const SoftwareCanvas& lhs, const SoftwareCanvas& rhs) {
  SDL_FlushRenderer(lhs.renderer());
  SDL_FlushRenderer(rhs.renderer());
  return CountPixelDifferences(lhs.surface(), rhs.surface());
}

// Paint exactly what `redraw` asks for onto `canvas`, as the app loop does
// (Application::RenderFrame): a full frame for `full`, otherwise one pass per
// rect under its clip WITH that clip as the dirty hint -- the hint is what lets
// RenderClip skip whole surfaces, and skipping the wrong one is a class of stale
// pixel only a hinted pass can show -- and NOTHING when it asks for nothing: an
// empty invalidation is a frame the app does not draw.
inline void PaintInvalidation(workspace::WorkspaceShell& shell,
                              SoftwareCanvas& canvas,
                              int width,
                              int height,
                              const workspace::RenderInvalidation& redraw) {
  if (redraw.full) {
    shell.Render(canvas.renderer(), width, height);
    return;
  }
  if (redraw.rects.empty()) {
    return;
  }
  const workspace::WorkspaceShell::FrameToken token =
      shell.PrepareFrameOnce(canvas.renderer(), width, height);
  for (const SDL_FRect& rect : redraw.rects) {
    const SDL_Rect clip = InflateClipRect(rect, shell.PartialRedrawClipPadding(), width, height);
    if (clip.w <= 0 || clip.h <= 0) {
      continue;
    }
    Expect(SDL_SetRenderClipRect(canvas.renderer(), &clip),
           "a retained redraw should set its clip rect");
    const SDL_FRect hint{static_cast<float>(clip.x), static_cast<float>(clip.y),
                         static_cast<float>(clip.w), static_cast<float>(clip.h)};
    shell.RenderClip(token, canvas.renderer(), width, height, hint);
  }
  Expect(SDL_SetRenderClipRect(canvas.renderer(), nullptr),
         "a retained redraw should clear its clip rect");
}

}  // namespace microide::tests
