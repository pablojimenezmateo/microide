#pragma once

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace microide::render {

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

}

#endif
