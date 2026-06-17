#include "editor/BreakpointRender.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace microide::editor {

SDL_FRect BreakpointGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                     float line_height) {
  // Diameter scales with line height but stays in a tidy range; the dot sits a
  // few px in from the gutter's left edge and centers vertically on the row.
  const float diameter = std::clamp(line_height * 0.55f, 6.0f, 12.0f);
  const float x = gutter_x + 4.0f;
  const float top = y + std::max(0.0f, (line_height - diameter) * 0.5f);
  return SDL_FRect{x, top, diameter, diameter};
}

void DrawBreakpointGutterMarker(SDL_Renderer* renderer, const render::Theme& theme, float gutter_x,
                                float y, float gutter_width, float line_height, bool verified) {
  if (renderer == nullptr || gutter_width <= 0.0f || line_height <= 0.0f) {
    return;
  }
  const SDL_FRect bounds = BreakpointGutterMarkerRect(gutter_x, y, gutter_width, line_height);
  const float radius = bounds.w * 0.5f;
  const float cx = bounds.x + radius;
  const float cy = bounds.y + radius;
  const SDL_Color color = verified ? theme.breakpoint : theme.breakpoint_unverified;
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

  // Fill the disc as horizontal spans (one rect per scanline). The dot is tiny
  // (≤12px), so this is a handful of fills coalesced by SDL's batcher.
  const int rows = static_cast<int>(std::ceil(bounds.h));
  std::vector<SDL_FRect> spans;
  spans.reserve(static_cast<std::size_t>(std::max(1, rows)));
  for (int i = 0; i < rows; ++i) {
    const float row_y = bounds.y + static_cast<float>(i);
    const float dy = (row_y + 0.5f) - cy;
    if (std::fabs(dy) > radius) {
      continue;
    }
    const float half_width = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
    spans.push_back(SDL_FRect{cx - half_width, row_y, half_width * 2.0f, 1.0f});
  }
  if (!spans.empty()) {
    SDL_RenderFillRects(renderer, spans.data(), static_cast<int>(spans.size()));
  }
}

}  // namespace microide::editor
