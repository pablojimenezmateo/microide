#include "TestSupport.h"

#include <cmath>
#include <string>
#include <vector>

#include "util/WindowPresentation.h"

namespace microide::tests {
namespace {

using microide::util::ComputeWindowPresentation;

void TestWindowPresentationUsesDisplayScaleForLogicalSize() {
  const auto presentation = ComputeWindowPresentation(3840, 2160, 2.0f, 1.0f);
  Expect(presentation.logical_width == 1920, "logical width should respect display scale");
  Expect(presentation.logical_height == 1080, "logical height should respect display scale");
  Expect(std::fabs(presentation.presentation_scale_x - 2.0f) < 0.001f,
         "horizontal presentation scale should match the resolved output scale");
  Expect(std::fabs(presentation.presentation_scale_y - 2.0f) < 0.001f,
         "vertical presentation scale should match the resolved output scale");
}

void TestWindowPresentationAppliesUiScaleMultiplier() {
  const auto presentation = ComputeWindowPresentation(3000, 1800, 1.5f, 2.0f);
  Expect(presentation.logical_width == 1000, "logical width should include ui scale");
  Expect(presentation.logical_height == 600, "logical height should include ui scale");
  Expect(std::fabs(presentation.presentation_scale_x - 3.0f) < 0.001f,
         "horizontal scale should reflect display and ui scale");
  Expect(std::fabs(presentation.presentation_scale_y - 3.0f) < 0.001f,
         "vertical scale should reflect display and ui scale");
}

void TestWindowPresentationSanitizesInvalidScales() {
  const auto presentation = ComputeWindowPresentation(800, 600, 0.0f, NAN);
  Expect(presentation.logical_width == 800, "invalid scales should fall back to 1x width");
  Expect(presentation.logical_height == 600, "invalid scales should fall back to 1x height");
  Expect(std::fabs(presentation.presentation_scale_x - 1.0f) < 0.001f,
         "invalid scales should fall back to 1x horizontal presentation scale");
  Expect(std::fabs(presentation.presentation_scale_y - 1.0f) < 0.001f,
         "invalid scales should fall back to 1x vertical presentation scale");
}

// A window that is not a whole number of units at the scale keeps the exact
// scale, its units rounded up to cover it. Stretched to 819 units, a 1024-pixel
// window at 1.25 drew at 1.2503, where no whole number of units is a whole number
// of device pixels and a surface's clip could not be aligned to a partial frame's
// (QA monkey, qa lane seed 1: the sidebar's text one device column into the
// editor in partial frames only).
void TestWindowPresentationKeepsTheExactScaleAtAnyWindowSize() {
  const auto presentation = ComputeWindowPresentation(1024, 899, 1.0f, 1.25f);
  Expect(presentation.presentation_scale_x == 1.25f && presentation.presentation_scale_y == 1.25f,
         "the presentation scale is exactly the requested one (" +
             std::to_string(presentation.presentation_scale_x) + ")");
  Expect(presentation.logical_width == 820 && presentation.logical_height == 720,
         "the units round up to cover the drawable (" + std::to_string(presentation.logical_width) + "x" +
             std::to_string(presentation.logical_height) + ")");
  const auto exact = ComputeWindowPresentation(1440, 900, 1.0f, 1.25f);
  Expect(exact.logical_width == 1152 && exact.logical_height == 720,
         "a window that is a whole number of units is that many");
}

// The window's minimum keeps the layout's floor at any ui.scale: at scale 3 a
// 640x400 window is a 213x133 layout, where the QA monkey found the chrome laid
// out off the window and over itself. Capped to the usable display area.
void TestMinimumWindowSizeKeepsTheLayoutFloorAtAnyScale() {
  using microide::util::MinimumWindowSize;
  using microide::util::WindowSize;
  const WindowSize one = MinimumWindowSize(1.0f, 1.0f, {});
  Expect(one.width == 640 && one.height == 400, "at scale 1 the floor is 640x400 window units");
  const WindowSize three = MinimumWindowSize(3.0f, 3.0f, {});
  Expect(three.width == 1920 && three.height == 1200, "at scale 3 the window must be three times as large");
  const WindowSize fractional = MinimumWindowSize(1.25f, 1.25f, {});
  Expect(fractional.width == 800 && fractional.height == 500, "a fractional scale rounds up");
  const WindowSize capped = MinimumWindowSize(3.0f, 3.0f, WindowSize{.width = 1440, .height = 900});
  Expect(capped.width == 1440 && capped.height == 900, "never more than the usable display area");
  const WindowSize sane = MinimumWindowSize(0.0f, NAN, {});
  Expect(sane.width == 640 && sane.height == 400, "an invalid factor falls back to 1");
}

}  // namespace

void RegisterWindowPresentationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WindowPresentation/UsesDisplayScaleForLogicalSize",
          TestWindowPresentationUsesDisplayScaleForLogicalSize);
  AddTest(tests, "WindowPresentation/AppliesUiScaleMultiplier",
          TestWindowPresentationAppliesUiScaleMultiplier);
  AddTest(tests, "WindowPresentation/KeepsTheExactScaleAtAnyWindowSize",
          TestWindowPresentationKeepsTheExactScaleAtAnyWindowSize);
  AddTest(tests, "WindowPresentation/SanitizesInvalidScales",
          TestWindowPresentationSanitizesInvalidScales);
  AddTest(tests, "WindowPresentation/MinimumWindowSizeKeepsTheLayoutFloorAtAnyScale",
          TestMinimumWindowSizeKeepsTheLayoutFloorAtAnyScale);
}

}  // namespace microide::tests
