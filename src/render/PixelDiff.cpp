#include "render/PixelDiff.h"

#include <algorithm>
#include <cstring>

namespace microide::render {

namespace {

constexpr int kBytesPerPixel = 4;

bool Comparable(const PixelImageView& image) {
  return image.pixels != nullptr && image.width > 0 && image.height > 0 &&
         image.pitch >= image.width * kBytesPerPixel;
}

}  // namespace

PixelDiffResult DiffPixels(const PixelImageView& lhs, const PixelImageView& rhs) {
  PixelDiffResult result;
  if (!Comparable(lhs) || !Comparable(rhs) || lhs.width != rhs.width || lhs.height != rhs.height) {
    result.size_mismatch = true;
    return result;
  }

  const std::size_t row_bytes = static_cast<std::size_t>(lhs.width) * kBytesPerPixel;
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < lhs.height; ++y) {
    const std::uint8_t* left = lhs.pixels + static_cast<std::size_t>(y) * lhs.pitch;
    const std::uint8_t* right = rhs.pixels + static_cast<std::size_t>(y) * rhs.pitch;
    if (std::memcmp(left, right, row_bytes) == 0) {
      continue;
    }
    for (int x = 0; x < lhs.width; ++x) {
      if (std::memcmp(left + x * kBytesPerPixel, right + x * kBytesPerPixel, kBytesPerPixel) == 0) {
        continue;
      }
      if (result.count == 0) {
        result.first_x = x;
        result.first_y = y;
        result.min_x = x;
        result.min_y = y;
      }
      ++result.count;
      result.min_x = std::min(result.min_x, x);
      max_x = std::max(max_x, x);
      max_y = y;
    }
  }
  if (result.count > 0) {
    result.box_width = max_x - result.min_x + 1;
    result.box_height = max_y - result.min_y + 1;
  }
  return result;
}

}  // namespace microide::render
