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
  // The units rounded up, and the scale exactly the requested one: the layout
  // then overhangs the drawable by under one unit, which its edge clips, instead
  // of being stretched to fit -- a stretch to a whole number of units made the
  // scale pixels / units (1024 / 819 = 1.2503 for 1.25), at which no whole number
  // of units is a whole number of device pixels (app/RenderPresentation.h).
  const auto units = [requested_scale](int pixels) {
    return std::max(1, static_cast<int>(std::ceil(static_cast<float>(pixels) / requested_scale - 1e-3f)));
  };
  presentation.logical_width = units(presentation.pixel_width);
  presentation.logical_height = units(presentation.pixel_height);
  presentation.presentation_scale_x = requested_scale;
  presentation.presentation_scale_y = requested_scale;
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

}  // namespace microide::util
