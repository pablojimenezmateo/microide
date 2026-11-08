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

// The smallest layout the workbench is laid out for, in layout units: below it
// the chrome ran off the window and over itself (QA monkey).
inline constexpr int kMinimumLayoutWidth = 640;
inline constexpr int kMinimumLayoutHeight = 400;

struct WindowSize {
  int width = 0;
  int height = 0;
};

// The window size (window coordinates) that keeps the layout at least
// kMinimumLayoutWidth x kMinimumLayoutHeight units when one layout unit is
// `window_per_unit_x` x `window_per_unit_y` window coordinates (ui.scale, on top
// of the display's density). Capped to `usable` (the display's usable area, when
// known: width/height > 0), since a floor the screen cannot hold would put the
// window off it; there the layout is as large as the screen allows.
WindowSize MinimumWindowSize(float window_per_unit_x, float window_per_unit_y, WindowSize usable);

}  // namespace microide::util
