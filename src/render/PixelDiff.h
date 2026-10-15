#pragma once

// A pure comparison of two 32-bit-per-pixel images.
//
// Two callers need the same answer -- "which pixels of this retained frame differ
// from a full render of the same state": the app's QA redraw verifier
// (app/RedrawVerifier, on every partial frame when MICROIDE_QA_VERIFY_REDRAW=1)
// and the unit suite's retained-redraw pixel tests (tests/support/RetainedRedraw.h).
// Both read back two images in ONE pixel format and compare them byte for byte;
// this is that comparison, with no SDL in it so it can be tested on plain buffers.

#include <cstddef>
#include <cstdint>

namespace microide::render {

// A read-only view of a 4-byte-per-pixel image. `pitch` is the byte stride of a
// row (>= width * 4). The two images of a comparison must share a pixel format;
// the format itself does not matter, since equality is bytewise.
struct PixelImageView {
  const std::uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int pitch = 0;
};

struct PixelDiffResult {
  // True when the images cannot be compared (different sizes, missing pixels, or
  // a pitch shorter than a row); every other field is then zero.
  bool size_mismatch = false;
  std::size_t count = 0;
  // Bounding box of the differing pixels as x, y, w, h (all zero when count == 0).
  int min_x = 0;
  int min_y = 0;
  int box_width = 0;
  int box_height = 0;
  // The first differing pixel in row-major order (-1, -1 when count == 0).
  int first_x = -1;
  int first_y = -1;

  bool identical() const { return !size_mismatch && count == 0; }
};

// Compare two images pixel by pixel. Rows that are bytewise equal are skipped
// with one memcmp, so a mostly-identical frame costs about a memcmp of the image.
PixelDiffResult DiffPixels(const PixelImageView& lhs, const PixelImageView& rhs);

}  // namespace microide::render
