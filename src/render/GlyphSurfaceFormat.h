#pragma once

#include <SDL3/SDL.h>

namespace microide::render {

// Glyph surfaces store *coverage*, and coverage lives in the alpha channel: a
// blended glyph render is the text colour at every pixel with the shape carried
// entirely by per-pixel alpha. Put one in a surface without an alpha channel and
// the shape is discarded, leaving an opaque rectangle of the text colour -- every
// character paints as a solid block.
//
// That is not hypothetical. Composites and the ASCII atlas are allocated in the
// renderer's own preferred texture format (so uploads do not swizzle), and the
// software renderer advertises SDL_PIXELFORMAT_XRGB8888 first. Under it the whole
// UI rendered as filled blocks, with only strings containing a non-ASCII byte --
// which bypass the atlas for the whole-string SDL_ttf path -- coming out right.
//
// So: keep the renderer's channel order, but never its missing alpha.

// The same format with an alpha channel. Returns `format` unchanged when it
// already has one; otherwise the matching alpha-carrying layout, falling back to
// RGBA32 for anything unrecognized (correct everywhere, at the cost of a swizzle
// on upload if the renderer wanted a different order).
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

// Picks the glyph-surface format from a renderer's advertised texture-format
// list (terminated by SDL_PIXELFORMAT_UNKNOWN). Prefers the earliest entry that
// already carries alpha -- the renderer's own preference order, minus the
// formats that cannot hold coverage -- and upgrades the first entry if none do.
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

}  // namespace microide::render
