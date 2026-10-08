#pragma once

#include <cstdint>

namespace microide::util {

// An 8-bit-per-channel RGBA colour. The kernel's colour type: the terminal model
// carries these in its cells, the shared ANSI palette returns them, and the render
// layer converts to the windowing library's own colour struct at its boundary
// (render::ToSdlColor). Layout matches SDL_Color, but the conversion is written out
// rather than reinterpreted so the two can diverge without silent corruption.
struct Rgba8 {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0xff;

  friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

// Dark by relative luminance (Rec. 709), the split terminal theme detection
// uses: what a program asking "is the background dark?" should be told.
constexpr bool IsDarkColor(Rgba8 color) {
  return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b < 128.0;
}

}  // namespace microide::util
