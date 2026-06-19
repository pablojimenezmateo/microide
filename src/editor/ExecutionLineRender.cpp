#include "editor/ExecutionLineRender.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "editor/GutterMetrics.h"

namespace microide::editor {

SDL_FRect ExecutionLineGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                        float line_height) {
  // Match the breakpoint dot's footprint so the arrow overlays it cleanly when a
  // session stops on a breakpoint, staying within the reserved marker strip.
  const float side = std::clamp(line_height * 0.55f, 6.0f, kGutterMarkerMaxExtent);
  const float x = gutter_x + kGutterMarkerInset;
  const float top = y + std::max(0.0f, (line_height - side) * 0.5f);
  return SDL_FRect{x, top, side, side};
}

void DrawExecutionLineGutterMarker(SDL_Renderer* renderer, const render::Theme& theme,
                                   float gutter_x, float y, float gutter_width, float line_height) {
  if (renderer == nullptr || gutter_width <= 0.0f || line_height <= 0.0f) {
    return;
  }
  const SDL_FRect bounds = ExecutionLineGutterMarkerRect(gutter_x, y, gutter_width, line_height);
  const SDL_Color color = theme.debug_execution_arrow;
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

  // Right-pointing isosceles triangle filled as horizontal spans. The triangle
  // occupies the middle ~60% of the bounds height; its tip is at the right edge,
  // its base on the left. Width shrinks linearly toward the tip.
  const float tip_x = bounds.x + bounds.w;
  const float base_x = bounds.x + bounds.w * 0.18f;
  const float half_h = bounds.h * 0.32f;
  const float cy = bounds.y + bounds.h * 0.5f;
  const int rows = static_cast<int>(std::ceil(half_h * 2.0f));
  std::vector<SDL_FRect> spans;
  spans.reserve(static_cast<std::size_t>(std::max(1, rows)));
  for (int i = 0; i < rows; ++i) {
    const float row_y = cy - half_h + static_cast<float>(i);
    const float dy = (row_y + 0.5f) - cy;
    if (std::fabs(dy) > half_h) {
      continue;
    }
    // Fraction from base (0) to tip (1) for this scanline's distance from center.
    const float t = half_h > 0.0f ? std::fabs(dy) / half_h : 0.0f;
    const float right = tip_x - (tip_x - base_x) * t;
    if (right <= base_x) {
      continue;
    }
    spans.push_back(SDL_FRect{base_x, row_y, right - base_x, 1.0f});
  }
  if (!spans.empty()) {
    SDL_RenderFillRects(renderer, spans.data(), static_cast<int>(spans.size()));
  }
}

}  // namespace microide::editor
