#pragma once

#include "render/PaintElement.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <cstdio>
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

inline void PaintInvalidation(workspace::WorkspaceShell& shell,
                              SoftwareCanvas& canvas,
                              int width,
                              int height,
                              const workspace::RenderInvalidation& redraw,
                              std::vector<SDL_FRect>* painted_rects = nullptr) {
  if (redraw.full) {
    shell.Render(canvas.renderer(), width, height);
    return;
  }
  if (redraw.rects.empty() && !redraw.frame) {
    return;
  }
  const workspace::WorkspaceShell::FrameToken token =
      shell.PrepareFrameOnce(canvas.renderer(), width, height);
  const workspace::RenderInvalidation prepared = shell.ConsumePendingRenderInvalidation();
  if (prepared.full) {
    shell.RenderClip(token, canvas.renderer(), width, height);
    return;
  }
  std::vector<SDL_FRect> rects(redraw.rects.begin(), redraw.rects.end());
  rects.insert(rects.end(), prepared.rects.begin(), prepared.rects.end());
  if (std::getenv("MQA_TEST_DUMP") != nullptr) {
    for (const SDL_FRect& r : prepared.rects) {
      std::fprintf(stderr, "derived: rect %.0f,%.0f %.0fx%.0f\n", r.x, r.y, r.w, r.h);
    }
  }
  if (painted_rects != nullptr) {
    painted_rects->insert(painted_rects->end(), rects.begin(), rects.end());
  }
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

inline workspace::RenderInvalidation FrameDamage(workspace::WorkspaceShell& shell,
                                                 SoftwareCanvas& canvas,
                                                 int width,
                                                 int height,
                                                 const workspace::RenderInvalidation& redraw) {
  workspace::RenderInvalidation all = redraw;
  (void)shell.PrepareFrameOnce(canvas.renderer(), width, height);
  all.Merge(shell.ConsumePendingRenderInvalidation());
  return all;
}

inline workspace::RenderInvalidation DispatchEvents(workspace::WorkspaceShell& shell,
                                                   std::initializer_list<SDL_Event> events) {
  workspace::RenderInvalidation redraw;
  for (const SDL_Event& event : events) {
    redraw.Merge(shell.HandleEvent(event).redraw);
    redraw.Merge(shell.ConsumePendingRenderInvalidation());
  }
  return redraw;
}

inline render::PixelDiffResult PaintAndDiffWithFullRepaint(
    workspace::WorkspaceShell& shell,
    SoftwareCanvas& canvas,
    int width,
    int height,
    const workspace::RenderInvalidation& redraw,
    const std::function<void()>& before_paint = {}) {
  const bool was_recording = render::PaintOverrunRecordingEnabled();
  render::SetPaintOverrunRecording(true);
  (void)render::TakePaintOverruns();
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
  render::SetPaintOverrunRecording(was_recording);
  for (const render::PaintOverrun& overrun : render::TakePaintOverruns()) {
    Expect(false, "element " + overrun.element + " at " + std::to_string(overrun.element_rect.x) + "," +
                      std::to_string(overrun.element_rect.y) + " " + std::to_string(overrun.element_rect.w) + "x" +
                      std::to_string(overrun.element_rect.h) + " drew outside it: " +
                      std::to_string(overrun.draw_rect.x) + "," + std::to_string(overrun.draw_rect.y) + " " +
                      std::to_string(overrun.draw_rect.w) + "x" + std::to_string(overrun.draw_rect.h));
  }
  const render::PixelDiffResult diff = DiffSurfaces(retained, canvas.surface());
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

}
