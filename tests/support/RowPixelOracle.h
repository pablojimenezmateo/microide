#pragma once

// Shared per-row pixel oracle for the editor/compare/merge decorated-row
// convergence. Renders a DecoratedTextRow (or a RowDecorationInput built through
// the unified builder) to an isolated software canvas and compares pixels, so a
// convergence step can be gated on byte-identical output against the legacy
// assembly it replaces. Header-only (all functions inline) so multiple test TUs
// can include it without ODR conflicts.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>

#include "TestSupport.h"
#include "editor/DecoratedTextGridRenderer.h"
#include "render/TextRenderer.h"

namespace microide::tests::oracle {

// Bring up the dummy SDL video driver exactly once per test binary. Redraw /
// pixel tests share this global SDL state and must run serial.
inline void EnsureDummyVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "row pixel oracle should initialize the dummy SDL video driver");
}

// An RGBA8888 software surface + renderer the oracle paints into and reads back.
class OracleCanvas final {
 public:
  OracleCanvas(int width, int height) : width_(width), height_(height) {
    surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    Expect(surface_ != nullptr, "row pixel oracle should allocate a software surface");
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    Expect(renderer_ != nullptr, "row pixel oracle should create a software renderer");
  }

  ~OracleCanvas() {
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (surface_ != nullptr) {
      SDL_DestroySurface(surface_);
    }
  }

  OracleCanvas(const OracleCanvas&) = delete;
  OracleCanvas& operator=(const OracleCanvas&) = delete;

  SDL_Renderer* renderer() const { return renderer_; }
  SDL_Surface* surface() const { return surface_; }
  int width() const { return width_; }
  int height() const { return height_; }

  void Clear(SDL_Color color) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer_);
  }

 private:
  int width_ = 0;
  int height_ = 0;
  SDL_Surface* surface_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
};

// Count pixels that differ between two equally-sized surfaces.
inline std::size_t CountPixelDifferences(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr, "pixel comparison needs readable surfaces");
  Expect(lhs->w == rhs->w && lhs->h == rhs->h,
         "pixel comparison needs matching canvas dimensions");
  std::size_t differences = 0;
  for (int y = 0; y < lhs->h; ++y) {
    for (int x = 0; x < lhs->w; ++x) {
      Uint8 lr = 0, lg = 0, lb = 0, la = 0;
      Uint8 rr = 0, rg = 0, rb = 0, ra = 0;
      Expect(SDL_ReadSurfacePixel(lhs, x, y, &lr, &lg, &lb, &la),
             "pixel comparison should read the actual surface");
      Expect(SDL_ReadSurfacePixel(rhs, x, y, &rr, &rg, &rb, &ra),
             "pixel comparison should read the reference surface");
      if (lr != rr || lg != rg || lb != rb || la != ra) {
        ++differences;
      }
    }
  }
  return differences;
}

// Render one decorated row onto a freshly-cleared canvas.
inline void PaintRow(OracleCanvas& canvas,
                     const render::TextRenderer& text_renderer,
                     const editor::DecoratedTextRow& row,
                     SDL_Color background) {
  static const editor::DecoratedTextGridRenderer kRowRenderer;
  canvas.Clear(background);
  kRowRenderer.RenderRow(canvas.renderer(), text_renderer, row);
}

// Convenience: assert two decorated rows paint byte-identically.
inline std::size_t RowPixelDifference(const render::TextRenderer& text_renderer,
                                      const editor::DecoratedTextRow& expected,
                                      const editor::DecoratedTextRow& actual,
                                      int width,
                                      int height,
                                      SDL_Color background) {
  OracleCanvas expected_canvas(width, height);
  OracleCanvas actual_canvas(width, height);
  PaintRow(expected_canvas, text_renderer, expected, background);
  PaintRow(actual_canvas, text_renderer, actual, background);
  return CountPixelDifferences(expected_canvas.surface(), actual_canvas.surface());
}

}  // namespace microide::tests::oracle
