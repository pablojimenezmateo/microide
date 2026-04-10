#pragma once

namespace microide::util {

struct WindowPresentation {
  int pixel_width = 0;
  int pixel_height = 0;
  int logical_width = 0;
  int logical_height = 0;
  float display_scale = 1.0f;
  float ui_scale = 1.0f;
  float presentation_scale_x = 1.0f;
  float presentation_scale_y = 1.0f;
};

WindowPresentation ComputeWindowPresentation(int pixel_width,
                                             int pixel_height,
                                             float display_scale,
                                             float ui_scale);

}  // namespace microide::util
