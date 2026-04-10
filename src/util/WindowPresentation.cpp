#include "util/WindowPresentation.h"

#include <algorithm>
#include <cmath>

namespace microide::util {

namespace {

constexpr float kMinScale = 0.1f;

float SanitizeScale(float scale) {
  return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

}  // namespace

WindowPresentation ComputeWindowPresentation(int pixel_width,
                                             int pixel_height,
                                             float display_scale,
                                             float ui_scale) {
  WindowPresentation presentation;
  presentation.pixel_width = std::max(1, pixel_width);
  presentation.pixel_height = std::max(1, pixel_height);
  presentation.display_scale = SanitizeScale(display_scale);
  presentation.ui_scale = SanitizeScale(ui_scale);

  const float requested_scale =
      std::max(kMinScale, presentation.display_scale * presentation.ui_scale);
  presentation.logical_width = std::max(
      1, static_cast<int>(std::lround(static_cast<float>(presentation.pixel_width) / requested_scale)));
  presentation.logical_height =
      std::max(1, static_cast<int>(std::lround(static_cast<float>(presentation.pixel_height) /
                                               requested_scale)));
  presentation.presentation_scale_x =
      static_cast<float>(presentation.pixel_width) / static_cast<float>(presentation.logical_width);
  presentation.presentation_scale_y = static_cast<float>(presentation.pixel_height) /
                                      static_cast<float>(presentation.logical_height);
  return presentation;
}

}  // namespace microide::util
