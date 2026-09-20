#pragma once

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace microide::render {

// Straight-copy one glyph's coverage out of a coverage atlas into `dst`, tinted
// to `color`, without going through SDL_BlitSurface.
//
// Worth its own path because of the CALL COUNT, not the pixel count: a rendered
// row string composites one of these per character, so a 200-column row is 200
// blitter dispatches back to back, and they measured 78.8 ms of the 84.5 ms that
// `editor_soft_wrap_long_line_scroll` spends rasterizing strings -- 93 %
// (TD-2026-08-30-276). Each individual glyph is ~8x17 pixels, far too small for
// SDL's per-blit setup to amortize.
//
// The arithmetic is an identity rather than an approximation, which is what makes
// this safe to substitute: a coverage atlas stores white coverage (RGB = 255,
// A = the glyph's alpha), and modulating that by an opaque colour is
// `255 * c / 255 == c` in every channel. So the result is a CONSTANT rgb word
// carrying the source's alpha byte, and there is nothing to multiply per pixel.
//
// Returns false -- leaving `dst` untouched -- for anything this cannot serve
// exactly, and the caller falls back to SDL_BlitSurface.
//
// Shared by AsciiGlyphAtlas and GlyphClusterAtlas: both store white coverage in
// the renderer's texture format, so the identity above holds for both, and a
// second copy of this loop is a second place for the clipping to be wrong.
inline bool TintCopyCoverage(const SDL_Surface* atlas, SDL_Surface* dst, int dst_x, int src_x,
                             int width, int height, SDL_Color color) {
  if (atlas == nullptr || dst == nullptr) {
    return false;
  }
  if (atlas->format != dst->format || SDL_MUSTLOCK(atlas) || SDL_MUSTLOCK(dst)) {
    return false;
  }
  const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(atlas->format);
  if (details == nullptr || details->bytes_per_pixel != 4 || details->Amask == 0) {
    return false;
  }
  // Clip to the destination the way SDL_BlitSurface's own clipping would, so a
  // glyph overhanging the composite's right edge is truncated rather than wrapping
  // into the next row of pixels.
  width = std::min(width, dst->w - dst_x);
  height = std::min(height, dst->h);
  if (dst_x < 0 || width <= 0 || height <= 0) {
    return false;
  }

  const std::uint32_t rgb = (static_cast<std::uint32_t>(color.r) << details->Rshift) |
                            (static_cast<std::uint32_t>(color.g) << details->Gshift) |
                            (static_cast<std::uint32_t>(color.b) << details->Bshift);
  const std::uint32_t alpha_mask = details->Amask;
  const auto* src_pixels = static_cast<const std::uint8_t*>(atlas->pixels);
  auto* dst_pixels = static_cast<std::uint8_t*>(dst->pixels);
  if (src_pixels == nullptr || dst_pixels == nullptr) {
    return false;
  }
  for (int y = 0; y < height; ++y) {
    const auto* src_row =
        reinterpret_cast<const std::uint32_t*>(
            src_pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(atlas->pitch)) +
        src_x;
    auto* dst_row = reinterpret_cast<std::uint32_t*>(
                        dst_pixels + static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(dst->pitch)) +
                    dst_x;
    for (int x = 0; x < width; ++x) {
      dst_row[x] = rgb | (src_row[x] & alpha_mask);
    }
  }
  return true;
}

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
