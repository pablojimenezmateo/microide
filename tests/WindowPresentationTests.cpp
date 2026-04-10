#include "TestSupport.h"

#include <cmath>
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

}  // namespace

void RegisterWindowPresentationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WindowPresentation/UsesDisplayScaleForLogicalSize",
          TestWindowPresentationUsesDisplayScaleForLogicalSize);
  AddTest(tests, "WindowPresentation/AppliesUiScaleMultiplier",
          TestWindowPresentationAppliesUiScaleMultiplier);
  AddTest(tests, "WindowPresentation/SanitizesInvalidScales",
          TestWindowPresentationSanitizesInvalidScales);
}

}  // namespace microide::tests
