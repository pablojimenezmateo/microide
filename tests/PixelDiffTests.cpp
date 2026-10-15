#include "TestSupport.h"

#include <cstdint>
#include <vector>

#include "render/PixelDiff.h"

namespace microide::tests {
namespace {

using render::DiffPixels;
using render::PixelDiffResult;
using render::PixelImageView;

struct Image {
  Image(int width, int height, int pitch_padding = 0)
      : width(width), height(height), pitch(width * 4 + pitch_padding),
        bytes(static_cast<std::size_t>(pitch) * height, 0x20) {}

  void Set(int x, int y, std::uint8_t value) {
    bytes[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4 + 1] = value;
  }

  PixelImageView View() const { return PixelImageView{bytes.data(), width, height, pitch}; }

  int width;
  int height;
  int pitch;
  std::vector<std::uint8_t> bytes;
};

void TestIdenticalImagesHaveNoDifference() {
  const Image lhs(17, 9);
  const Image rhs(17, 9);
  const PixelDiffResult diff = DiffPixels(lhs.View(), rhs.View());
  Expect(diff.identical(), "identical images should compare identical");
  Expect(diff.count == 0 && diff.box_width == 0 && diff.box_height == 0,
         "identical images should report no bounding box");
  Expect(diff.first_x == -1 && diff.first_y == -1,
         "identical images should report no first differing pixel");
}

void TestOnePixelIsLocated() {
  const Image lhs(17, 9);
  Image rhs(17, 9);
  rhs.Set(5, 3, 0xff);
  const PixelDiffResult diff = DiffPixels(lhs.View(), rhs.View());
  Expect(diff.count == 1, "one changed pixel should count once");
  Expect(diff.min_x == 5 && diff.min_y == 3 && diff.box_width == 1 && diff.box_height == 1,
         "one changed pixel should be its own bounding box");
  Expect(diff.first_x == 5 && diff.first_y == 3, "one changed pixel should be the first");
}

void TestBoundingBoxSpansScatteredPixels() {
  const Image lhs(20, 12);
  Image rhs(20, 12);
  rhs.Set(9, 2, 1);
  rhs.Set(3, 7, 1);
  rhs.Set(15, 10, 1);
  const PixelDiffResult diff = DiffPixels(lhs.View(), rhs.View());
  Expect(diff.count == 3, "three changed pixels should count three");
  Expect(diff.min_x == 3 && diff.min_y == 2 && diff.box_width == 13 && diff.box_height == 9,
         "the bounding box should span the leftmost, topmost, rightmost and lowest pixels");
  Expect(diff.first_x == 9 && diff.first_y == 2,
         "the first differing pixel is the first in row-major order");
}

void TestRowPaddingIsIgnored() {
  Image lhs(8, 4, 12);
  Image rhs(8, 4, 4);
  // Bytes past the row (pitch padding) differ between the images and must not count.
  lhs.bytes[static_cast<std::size_t>(lhs.width) * 4] = 0x99;
  const PixelDiffResult diff = DiffPixels(lhs.View(), rhs.View());
  Expect(diff.identical(), "pitch padding should not take part in the comparison");
}

void TestSizeMismatchIsNotComparable() {
  const Image lhs(8, 4);
  const Image rhs(8, 5);
  const PixelDiffResult diff = DiffPixels(lhs.View(), rhs.View());
  Expect(diff.size_mismatch && !diff.identical(), "different sizes should not compare");
  Expect(DiffPixels(PixelImageView{}, rhs.View()).size_mismatch,
         "a missing image should not compare");
  Expect(DiffPixels(PixelImageView{lhs.bytes.data(), 8, 4, 8}, lhs.View()).size_mismatch,
         "a pitch shorter than a row should not compare");
}

}  // namespace

void RegisterPixelDiffTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PixelDiff/IdenticalImagesHaveNoDifference", TestIdenticalImagesHaveNoDifference);
  AddTest(tests, "PixelDiff/OnePixelIsLocated", TestOnePixelIsLocated);
  AddTest(tests, "PixelDiff/BoundingBoxSpansScatteredPixels", TestBoundingBoxSpansScatteredPixels);
  AddTest(tests, "PixelDiff/RowPaddingIsIgnored", TestRowPaddingIsIgnored);
  AddTest(tests, "PixelDiff/SizeMismatchIsNotComparable", TestSizeMismatchIsNotComparable);
}

}  // namespace microide::tests
