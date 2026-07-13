#include "editor/GutterIconRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "editor/GutterMetrics.h"

namespace microide::editor {

namespace {

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

// Fill a centered shape row-by-row. `half_width_at(dy)` returns the outer
// half-extent at vertical offset `dy` from the shape center; `inner_at` (may be
// null) carves a concentric hole (for rings). Coalesced by SDL's batcher; the
// marker is ≤12px so this is a handful of fills.
template <typename Outer, typename Inner>
void FillScanlines(SDL_Renderer* renderer, const SDL_FRect& bounds, float cx, float cy, Outer outer,
                   Inner inner) {
  const int rows = static_cast<int>(std::ceil(bounds.h));
  // Reuse per-thread scratch: gutter icons are drawn per marked line per frame, so
  // a fresh vector each call is a hot-path heap allocation. Clearing keeps capacity.
  thread_local std::vector<SDL_FRect> spans;
  spans.clear();
  spans.reserve(static_cast<std::size_t>(std::max(1, rows)) * 2);
  for (int i = 0; i < rows; ++i) {
    const float row_y = bounds.y + static_cast<float>(i);
    const float dy = (row_y + 0.5f) - cy;
    const float o = outer(dy);
    if (o <= 0.0f) {
      continue;
    }
    const float in = inner(dy);
    if (in <= 0.0f) {
      spans.push_back(SDL_FRect{cx - o, row_y, o * 2.0f, 1.0f});
    } else {
      spans.push_back(SDL_FRect{cx - o, row_y, o - in, 1.0f});
      spans.push_back(SDL_FRect{cx + in, row_y, o - in, 1.0f});
    }
  }
  if (!spans.empty()) {
    SDL_RenderFillRects(renderer, spans.data(), static_cast<int>(spans.size()));
  }
}

void DrawCheck(SDL_Renderer* renderer, const SDL_FRect& b, SDL_Color color) {
  const SDL_FColor fc{static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                      static_cast<float>(color.b) / 255.0f, static_cast<float>(color.a) / 255.0f};
  const float thickness = std::clamp(b.w * 0.18f, 1.5f, 3.0f);
  const std::array<SDL_FPoint, 3> pts{
      SDL_FPoint{b.x + b.w * 0.16f, b.y + b.h * 0.54f},
      SDL_FPoint{b.x + b.w * 0.42f, b.y + b.h * 0.78f},
      SDL_FPoint{b.x + b.w * 0.84f, b.y + b.h * 0.26f},
  };
  // Reused per-thread scratch: DrawCheck runs per checkmark gutter icon per frame.
  thread_local std::vector<SDL_Vertex> verts;
  thread_local std::vector<int> indices;
  verts.clear();
  indices.clear();
  const auto add_segment = [&](SDL_FPoint p0, SDL_FPoint p1) {
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f) {
      return;
    }
    dx /= len;
    dy /= len;
    const float nx = -dy * thickness * 0.5f;
    const float ny = dx * thickness * 0.5f;
    const int base = static_cast<int>(verts.size());
    verts.push_back(SDL_Vertex{SDL_FPoint{p0.x + nx, p0.y + ny}, fc, {}});
    verts.push_back(SDL_Vertex{SDL_FPoint{p0.x - nx, p0.y - ny}, fc, {}});
    verts.push_back(SDL_Vertex{SDL_FPoint{p1.x - nx, p1.y - ny}, fc, {}});
    verts.push_back(SDL_Vertex{SDL_FPoint{p1.x + nx, p1.y + ny}, fc, {}});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
  };
  add_segment(pts[0], pts[1]);
  add_segment(pts[1], pts[2]);
  if (!verts.empty()) {
    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
  }
}

}  // namespace

std::optional<GutterIconShape> GutterIconRegistry::ResolveShape(std::string_view name) {
  struct Entry {
    std::string_view name;
    GutterIconShape shape;
  };
  static constexpr std::array<Entry, 9> kEntries{{
      {"dot", GutterIconShape::Dot},
      {"circle", GutterIconShape::Circle},
      {"diamond", GutterIconShape::Diamond},
      {"triangle", GutterIconShape::Triangle},
      {"bookmark", GutterIconShape::Bookmark},
      {"check", GutterIconShape::Check},
      {"checkmark", GutterIconShape::Check},
      {"dash", GutterIconShape::Dash},
      {"square", GutterIconShape::Square},
  }};
  for (const Entry& entry : kEntries) {
    if (EqualsIgnoreAsciiCase(name, entry.name)) {
      return entry.shape;
    }
  }
  return std::nullopt;
}

SDL_FRect GutterIconRegistry::MarkerRect(float gutter_x, float y, float /*gutter_width*/,
                                         float line_height) {
  const float diameter = std::clamp(line_height * 0.55f, 6.0f, kGutterMarkerMaxExtent);
  const float x = gutter_x + kGutterMarkerInset;
  const float top = y + std::max(0.0f, (line_height - diameter) * 0.5f);
  return SDL_FRect{x, top, diameter, diameter};
}

void GutterIconRegistry::Draw(SDL_Renderer* renderer, GutterIconShape shape, SDL_Color color,
                              float gutter_x, float y, float gutter_width, float line_height) {
  if (renderer == nullptr || gutter_width <= 0.0f || line_height <= 0.0f || color.a == 0) {
    return;
  }
  const SDL_FRect bounds = MarkerRect(gutter_x, y, gutter_width, line_height);
  const float radius = bounds.w * 0.5f;
  const float cx = bounds.x + radius;
  const float cy = bounds.y + radius;
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

  const auto no_inner = [](float) { return 0.0f; };
  switch (shape) {
    case GutterIconShape::Dot:
    case GutterIconShape::Circle: {
      const auto disc = [&](float dy) {
        return std::fabs(dy) > radius ? 0.0f : std::sqrt(std::max(0.0f, radius * radius - dy * dy));
      };
      if (shape == GutterIconShape::Dot) {
        FillScanlines(renderer, bounds, cx, cy, disc, no_inner);
      } else {
        const float inner_radius = std::max(0.0f, radius - std::clamp(radius * 0.45f, 1.0f, 2.5f));
        const auto inner = [&](float dy) {
          return std::fabs(dy) > inner_radius
                     ? 0.0f
                     : std::sqrt(std::max(0.0f, inner_radius * inner_radius - dy * dy));
        };
        FillScanlines(renderer, bounds, cx, cy, disc, inner);
      }
      break;
    }
    case GutterIconShape::Diamond: {
      const auto diamond = [&](float dy) { return std::max(0.0f, radius - std::fabs(dy)); };
      FillScanlines(renderer, bounds, cx, cy, diamond, no_inner);
      break;
    }
    case GutterIconShape::Triangle: {
      // Upward triangle: apex at top, base at bottom.
      const auto tri = [&](float dy) {
        const float frac = (dy + radius) / bounds.h;  // 0 at top, 1 at bottom
        return std::clamp(frac, 0.0f, 1.0f) * radius;
      };
      FillScanlines(renderer, bounds, cx, cy, tri, no_inner);
      break;
    }
    case GutterIconShape::Bookmark: {
      // Pennant: full-width rect with a V-notch carved from the bottom center.
      const float notch_height = bounds.h * 0.32f;
      const float notch_top = bounds.y + bounds.h - notch_height;
      const auto outer = [&](float) { return radius; };
      const auto inner = [&](float dy) {
        const float row_y = cy + dy;
        if (row_y < notch_top) {
          return 0.0f;
        }
        const float db = (bounds.y + bounds.h) - row_y;  // distance from bottom
        return std::clamp(db / notch_height, 0.0f, 1.0f) * radius;
      };
      FillScanlines(renderer, bounds, cx, cy, outer, inner);
      break;
    }
    case GutterIconShape::Dash: {
      const float h = std::max(2.0f, bounds.h * 0.24f);
      SDL_FRect bar{bounds.x, cy - h * 0.5f, bounds.w, h};
      SDL_RenderFillRect(renderer, &bar);
      break;
    }
    case GutterIconShape::Square: {
      // Slightly inset filled square so it reads distinctly from the dot.
      const float inset = std::max(0.0f, bounds.w * 0.08f);
      SDL_FRect box{bounds.x + inset, bounds.y + inset, bounds.w - inset * 2.0f,
                    bounds.h - inset * 2.0f};
      SDL_RenderFillRect(renderer, &box);
      break;
    }
    case GutterIconShape::Check:
      DrawCheck(renderer, bounds, color);
      break;
  }
}

}  // namespace microide::editor
