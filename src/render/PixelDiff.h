#pragma once

#include <cstddef>
#include <cstdint>

namespace microide::render {

struct PixelImageView {
  const std::uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int pitch = 0;
};

struct PixelDiffResult {
  bool size_mismatch = false;
  std::size_t count = 0;
  int min_x = 0;
  int min_y = 0;
  int box_width = 0;
  int box_height = 0;
  int first_x = -1;
  int first_y = -1;

  bool identical() const { return !size_mismatch && count == 0; }
};

PixelDiffResult DiffPixels(const PixelImageView& lhs, const PixelImageView& rhs);

}
