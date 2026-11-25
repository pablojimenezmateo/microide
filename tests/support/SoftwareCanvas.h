#pragma once

#include <SDL3/SDL.h>

#include "TestSupport.h"

namespace microide::tests {

class SoftwareCanvas final {
 public:
  SoftwareCanvas(int width, int height) {
    surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    Expect(surface_ != nullptr, "render test should allocate a software surface");
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    Expect(renderer_ != nullptr, "render test should create a software renderer");
  }

  SoftwareCanvas(const SoftwareCanvas&) = delete;
  SoftwareCanvas& operator=(const SoftwareCanvas&) = delete;

  ~SoftwareCanvas() {
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (surface_ != nullptr) {
      SDL_DestroySurface(surface_);
    }
  }

  SDL_Renderer* renderer() const { return renderer_; }
  SDL_Surface* surface() const { return surface_; }

  SDL_Color PixelAt(int x, int y) const {
    SDL_FlushRenderer(renderer_);
    SDL_Color color{};
    Expect(SDL_ReadSurfacePixel(surface_, x, y, &color.r, &color.g, &color.b, &color.a),
           "reading a canvas pixel should succeed");
    return color;
  }

 private:
  SDL_Surface* surface_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
};

}
