#pragma once

#include <cstdint>

namespace microide::util {

struct Rgba8 {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0xff;

  friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

constexpr bool IsDarkColor(Rgba8 color) {
  return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b < 128.0;
}

}
