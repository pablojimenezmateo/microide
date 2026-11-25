#include "editor/BreakpointRender.h"
#include "render/SurfacePrimitives.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "editor/GutterMetrics.h"

namespace microide::editor {

SDL_FRect BreakpointGutterMarkerRect(float gutter_x, float y, float ,
                                     float line_height) {
  const float diameter = std::clamp(line_height * 0.55f, 6.0f, kGutterMarkerMaxExtent);
  const float x = gutter_x + kGutterMarkerInset;
  const float top = y + std::max(0.0f, (line_height - diameter) * 0.5f);
  return SDL_FRect{x, top, diameter, diameter};
}

void DrawBreakpointGutterMarker(SDL_Renderer* renderer, const render::Theme& theme, float gutter_x,
                                float y, float gutter_width, float line_height, bool verified,
                                BreakpointGutterKind kind, bool enabled) {
  if (renderer == nullptr || gutter_width <= 0.0f || line_height <= 0.0f) {
    return;
  }
  const SDL_FRect bounds = BreakpointGutterMarkerRect(gutter_x, y, gutter_width, line_height);
  const float radius = bounds.w * 0.5f;
  const float cx = bounds.x + radius;
  const float cy = bounds.y + radius;

  SDL_Color color = verified ? theme.breakpoint : theme.breakpoint_unverified;
  if (kind == BreakpointGutterKind::Conditional) {
    color = theme.breakpoint_conditional;
    if (!verified) {
      color.a = static_cast<Uint8>(static_cast<float>(color.a) * 0.6f);
    }
  }
  render::SetDrawColor(renderer, color);

  const bool diamond = kind == BreakpointGutterKind::Logpoint;
  const float thickness = enabled ? 0.0f : std::clamp(radius * 0.45f, 1.0f, 2.5f);
  const float inner_radius = radius - thickness;
  const auto half_width_at = [&](float dy, float r) {
    if (r <= 0.0f || std::fabs(dy) > r) {
      return 0.0f;
    }
    return diamond ? std::max(0.0f, r - std::fabs(dy))
                   : std::sqrt(std::max(0.0f, r * r - dy * dy));
  };
  const int rows = static_cast<int>(std::ceil(bounds.h));
  std::vector<SDL_FRect> spans;
  spans.reserve(static_cast<std::size_t>(std::max(1, rows)) * (enabled ? 1 : 2));
  for (int i = 0; i < rows; ++i) {
    const float row_y = bounds.y + static_cast<float>(i);
    const float dy = (row_y + 0.5f) - cy;
    const float outer = half_width_at(dy, radius);
    if (outer <= 0.0f) {
      continue;
    }
    const float inner = enabled ? 0.0f : half_width_at(dy, inner_radius);
    if (inner <= 0.0f) {
      spans.push_back(SDL_FRect{cx - outer, row_y, outer * 2.0f, 1.0f});
    } else {
      spans.push_back(SDL_FRect{cx - outer, row_y, outer - inner, 1.0f});
      spans.push_back(SDL_FRect{cx + inner, row_y, outer - inner, 1.0f});
    }
  }
  if (!spans.empty()) {
    render::FillRectsInDrawColor(renderer, spans.data(), static_cast<int>(spans.size()));
  }
}

}
