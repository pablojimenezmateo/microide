#include "util/WindowPresentation.h"

#include <algorithm>
#include <cmath>

namespace microide::util {

namespace {

constexpr float kMinScale = 0.1f;

constexpr float kFittedScaleStep = 0.05f;

float SanitizeScale(float scale) {
  return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

float ScaleThatHoldsTheLayoutFloor(float requested_scale, float display_scale, int pixel_width, int pixel_height) {
  const float fit = std::min(static_cast<float>(pixel_width) / static_cast<float>(kMinimumLayoutWidth),
                             static_cast<float>(pixel_height) / static_cast<float>(kMinimumLayoutHeight));
  if (fit >= requested_scale) {
    return requested_scale;
  }
  const float stepped = std::floor(fit / kFittedScaleStep) * kFittedScaleStep;
  return std::max(stepped, std::min(requested_scale, std::max(kMinScale, display_scale)));
}

}

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
  const float scale = ScaleThatHoldsTheLayoutFloor(requested_scale, presentation.display_scale,
                                                   presentation.pixel_width, presentation.pixel_height);
  const auto units = [scale](int pixels) {
    return std::max(1, static_cast<int>(std::ceil(static_cast<float>(pixels) / scale - 1e-3f)));
  };
  presentation.logical_width = units(presentation.pixel_width);
  presentation.logical_height = units(presentation.pixel_height);
  presentation.presentation_scale_x = scale;
  presentation.presentation_scale_y = scale;
  presentation.requested_scale = requested_scale;
  return presentation;
}

WindowSize MinimumWindowSize(float window_per_unit_x, float window_per_unit_y, WindowSize usable) {
  const auto per_unit = [](float value) { return std::isfinite(value) && value > 0.0f ? value : 1.0f; };
  WindowSize size{
      .width = static_cast<int>(std::ceil(static_cast<float>(kMinimumLayoutWidth) * per_unit(window_per_unit_x))),
      .height = static_cast<int>(std::ceil(static_cast<float>(kMinimumLayoutHeight) * per_unit(window_per_unit_y))),
  };
  if (usable.width > 0) {
    size.width = std::min(size.width, usable.width);
  }
  if (usable.height > 0) {
    size.height = std::min(size.height, usable.height);
  }
  return size;
}

}
