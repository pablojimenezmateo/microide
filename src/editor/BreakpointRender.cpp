#include "editor/BreakpointRender.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "editor/GutterMetrics.h"

namespace microide::editor {

SDL_FRect BreakpointGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                     float line_height) {
  // Diameter scales with line height but stays within the reserved marker strip;
  // the dot sits a few px in from the gutter's left edge, left of the line numbers
  // (which begin at kGutterLineNumberInset), and centers vertically on the row.
  const float diameter = std::clamp(line_height * 0.55f, 6.0f, kGutterMarkerMaxExtent);
  const float x = gutter_x + kGutterMarkerInset;
  const float top = y + std::max(0.0f, (line_height - diameter) * 0.5f);
  return SDL_FRect{x, top, diameter, diameter};
}

void DrawBreakpointGutterMarker(SDL_Renderer* renderer, const render::Theme& theme, float gutter_x,
                                float y, float gutter_width, float line_height, bool verified,
                                BreakpointGutterKind kind) {
  if (renderer == nullptr || gutter_width <= 0.0f || line_height <= 0.0f) {
    return;
  }
  const SDL_FRect bounds = BreakpointGutterMarkerRect(gutter_x, y, gutter_width, line_height);
  const float radius = bounds.w * 0.5f;
  const float cx = bounds.x + radius;
  const float cy = bounds.y + radius;

  // Conditional / hit-count breakpoints keep the disc but swap to a distinct tint
  // (dimmed when not yet verified). Logpoints switch to a diamond outline below.
  SDL_Color color = verified ? theme.breakpoint : theme.breakpoint_unverified;
  if (kind == BreakpointGutterKind::Conditional) {
    color = theme.breakpoint_conditional;
    if (!verified) {
      color.a = static_cast<Uint8>(static_cast<float>(color.a) * 0.6f);
    }
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

  // Fill the shape as horizontal spans (one rect per scanline). The marker is tiny
  // (≤12px), so this is a handful of fills coalesced by SDL's batcher. A disc uses
  // a circular half-width; a logpoint diamond uses a linear one (peaks at center).
  const bool diamond = kind == BreakpointGutterKind::Logpoint;
  const int rows = static_cast<int>(std::ceil(bounds.h));
  std::vector<SDL_FRect> spans;
  spans.reserve(static_cast<std::size_t>(std::max(1, rows)));
  for (int i = 0; i < rows; ++i) {
    const float row_y = bounds.y + static_cast<float>(i);
    const float dy = (row_y + 0.5f) - cy;
    if (std::fabs(dy) > radius) {
      continue;
    }
    const float half_width = diamond ? std::max(0.0f, radius - std::fabs(dy))
                                      : std::sqrt(std::max(0.0f, radius * radius - dy * dy));
    spans.push_back(SDL_FRect{cx - half_width, row_y, half_width * 2.0f, 1.0f});
  }
  if (!spans.empty()) {
    SDL_RenderFillRects(renderer, spans.data(), static_cast<int>(spans.size()));
  }
}

}  // namespace microide::editor
