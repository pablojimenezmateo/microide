#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace microide::render {

struct SurfaceDeleter {
  void operator()(SDL_Surface* surface) const noexcept { SDL_DestroySurface(surface); }
};

using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

}
