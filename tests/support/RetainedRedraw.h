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
#include <functional>
#include <cstdlib>
#include <string>
#include <vector>
#include <initializer_list>

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

// The same comparison the app's QA redraw verifier runs (render/PixelDiff). It is
// bytewise, so two surfaces of different formats (a TTF render against an RGBA32
// canvas) are compared after converting the right one to the left one's format.
inline render::PixelDiffResult DiffSurfaces(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr, "pixel comparisons should receive readable surfaces");
  Expect(SDL_BYTESPERPIXEL(lhs->format) == 4 && !SDL_MUSTLOCK(lhs),
         "pixel comparisons should read a 4-byte surface that needs no lock");
  SDL_Surface* converted = nullptr;
  if (rhs->format != lhs->format) {
    converted = SDL_ConvertSurface(rhs, lhs->format);
    Expect(converted != nullptr, "pixel comparisons should convert the reference surface");
    rhs = converted;
  }
  const render::PixelDiffResult diff = render::DiffPixels(
      render::PixelImageView{static_cast<const std::uint8_t*>(lhs->pixels), lhs->w, lhs->h,
                             lhs->pitch},
      render::PixelImageView{static_cast<const std::uint8_t*>(rhs->pixels), rhs->w, rhs->h,
                             rhs->pitch});
  SDL_DestroySurface(converted);
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
// pixel only a hinted pass can show -- plus whatever preparing the frame asked
// for, and NOTHING when it asks for nothing: an empty invalidation is a frame the
// app does not draw.
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
  // Damage requested while preparing the frame is painted in it, as
  // Application::Render does.
  const workspace::RenderInvalidation prepared = shell.ConsumePendingRenderInvalidation();
  if (prepared.full) {
    shell.RenderClip(token, canvas.renderer(), width, height);
    return;
  }
  std::vector<SDL_FRect> rects(redraw.rects.begin(), redraw.rects.end());
  rects.insert(rects.end(), prepared.rects.begin(), prepared.rects.end());
  for (const SDL_FRect& rect : rects) {
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

// The app loop's handling of a run of events: each event's own redraw folded with
// whatever the shell queued while handling it. A test that keeps only `handled`
// paints nothing, which is how a click-path test once passed vacuously.
inline workspace::RenderInvalidation DispatchEvents(workspace::WorkspaceShell& shell,
                                                   std::initializer_list<SDL_Event> events) {
  workspace::RenderInvalidation redraw;
  for (const SDL_Event& event : events) {
    redraw.Merge(shell.HandleEvent(event).redraw);
    redraw.Merge(shell.ConsumePendingRenderInvalidation());
  }
  return redraw;
}

// Paint `redraw` over the retained canvas as the app would, then repaint the same
// state in full ON THE SAME CANVAS (glyph textures are per renderer, so a fresh
// canvas differs wherever there is text) and diff the two. The canvas is left
// holding the full repaint, so steps chain. `before_paint` runs before each paint
// -- tests pin the caret blink there so both paints agree on it.
inline render::PixelDiffResult PaintAndDiffWithFullRepaint(
    workspace::WorkspaceShell& shell,
    SoftwareCanvas& canvas,
    int width,
    int height,
    const workspace::RenderInvalidation& redraw,
    const std::function<void()>& before_paint = {}) {
  if (before_paint) {
    before_paint();
  }
  PaintInvalidation(shell, canvas, width, height, redraw);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
  Expect(retained != nullptr, "a retained-redraw comparison should copy the canvas");
  if (before_paint) {
    before_paint();
  }
  shell.Render(canvas.renderer(), width, height);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_FlushRenderer(canvas.renderer());
  const render::PixelDiffResult diff = DiffSurfaces(retained, canvas.surface());
  // MICROIDE_TEST_DUMP_DIFF=<dir>: the two images of a differing pair, for reading
  // a failure by eye (the QA harness keeps the same pair per stale frame).
  if (const char* dump = std::getenv("MICROIDE_TEST_DUMP_DIFF"); dump != nullptr && diff.count != 0) {
    static int dump_index = 0;
    const std::string base = std::string(dump) + "/diff-" + std::to_string(dump_index++);
    SDL_SaveBMP(retained, (base + "-retained.bmp").c_str());
    SDL_SaveBMP(canvas.surface(), (base + "-reference.bmp").c_str());
  }
  SDL_DestroySurface(retained);
  return diff;
}

inline std::string DescribePixelDiff(const render::PixelDiffResult& diff) {
  return std::to_string(diff.count) + " pixels differ in " + std::to_string(diff.min_x) + "," +
         std::to_string(diff.min_y) + " " + std::to_string(diff.box_width) + "x" +
         std::to_string(diff.box_height);
}

}  // namespace microide::tests
