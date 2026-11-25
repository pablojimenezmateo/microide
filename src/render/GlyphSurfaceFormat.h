#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

inline SDL_PixelFormat EnsureAlphaCapableFormat(SDL_PixelFormat format) {
  if (const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(format);
      details != nullptr && details->Amask != 0) {
    return format;
  }
  switch (format) {
    case SDL_PIXELFORMAT_XRGB8888:
      return SDL_PIXELFORMAT_ARGB8888;
    case SDL_PIXELFORMAT_XBGR8888:
      return SDL_PIXELFORMAT_ABGR8888;
    case SDL_PIXELFORMAT_RGBX8888:
      return SDL_PIXELFORMAT_RGBA8888;
    case SDL_PIXELFORMAT_BGRX8888:
      return SDL_PIXELFORMAT_BGRA8888;
    default:
      return SDL_PIXELFORMAT_RGBA32;
  }
}

inline SDL_PixelFormat ChooseGlyphSurfaceFormat(const SDL_PixelFormat* renderer_formats) {
  if (renderer_formats == nullptr || renderer_formats[0] == SDL_PIXELFORMAT_UNKNOWN) {
    return SDL_PIXELFORMAT_RGBA32;
  }
  for (int i = 0; renderer_formats[i] != SDL_PIXELFORMAT_UNKNOWN; ++i) {
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(renderer_formats[i]);
    if (details != nullptr && details->Amask != 0) {
      return renderer_formats[i];
    }
  }
  return EnsureAlphaCapableFormat(renderer_formats[0]);
}

}
