#include "util/AnsiPalette.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace microide::util {

namespace {

constexpr Rgba8 MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                          std::uint8_t a = 0xff) {
  return Rgba8{r, g, b, a};
}

}  // namespace

Rgba8 BasicAnsiColor(int index, bool bright) {
  static constexpr std::array<Rgba8, 8> kNormal = {
      MakeColor(0x1f, 0x24, 0x2c), MakeColor(0xc3, 0x4b, 0x59),
      MakeColor(0x8a, 0xb1, 0x66), MakeColor(0xd8, 0xb2, 0x5d),
      MakeColor(0x5a, 0x8c, 0xe6), MakeColor(0xb0, 0x72, 0xd1),
      MakeColor(0x56, 0xa8, 0xc9), MakeColor(0xb8, 0xc0, 0xcc),
  };
  static constexpr std::array<Rgba8, 8> kBright = {
      MakeColor(0x4a, 0x51, 0x5c), MakeColor(0xf0, 0x71, 0x78),
      MakeColor(0xa4, 0xc7, 0x6d), MakeColor(0xe7, 0xc5, 0x47),
      MakeColor(0x72, 0xa7, 0xff), MakeColor(0xcb, 0x8f, 0xf8),
      MakeColor(0x74, 0xc7, 0xec), MakeColor(0xf5, 0xf7, 0xfa),
  };
  const int clamped_index = std::clamp(index, 0, 7);
  return bright ? kBright[clamped_index] : kNormal[clamped_index];
}

Rgba8 Ansi256Color(int index) {
  // The 256-colour palette is only defined for 0..255. Out-of-range indices
  // (e.g. an over-large theme token or an SGR 38;5;N with N > 255) would otherwise
  // fall through to the unclamped grayscale ramp and wrap via uint8 truncation.
  if (index < 0 || index > 255) {
    return BasicAnsiColor(0, false);
  }
  if (index < 8) {
    return BasicAnsiColor(index, false);
  }
  if (index < 16) {
    return BasicAnsiColor(index - 8, true);
  }
  if (index < 232) {
    const int value = index - 16;
    const int red = value / 36;
    const int green = (value / 6) % 6;
    const int blue = value % 6;
    static constexpr std::array<std::uint8_t, 6> kCube = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    return MakeColor(kCube[red], kCube[green], kCube[blue]);
  }
  const std::uint8_t gray = static_cast<std::uint8_t>(8 + (index - 232) * 10);
  return MakeColor(gray, gray, gray);
}

}  // namespace microide::util
