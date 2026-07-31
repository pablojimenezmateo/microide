#include "workspace/OverviewRuler.h"
#include "render/SurfacePrimitives.h"

#include <algorithm>
#include <cmath>

#include "render/Theme.h"

namespace microide::workspace::overview {

namespace {

SDL_FRect MakeRect(float x, float y, float w, float h) { return SDL_FRect{x, y, w, h}; }

bool SameColor(const SDL_Color& a, const SDL_Color& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

}  // namespace

SDL_FRect LaneRect(const SDL_FRect& track, float min_x) {
  const float x = std::max(min_x, track.x - kLaneGap - kLaneWidth);
  return MakeRect(x, track.y, kLaneWidth, track.h);
}

SDL_FRect LaneInnerRect(const SDL_FRect& lane) {
  return MakeRect(lane.x + 1.0f, lane.y + 1.0f, std::max(0.0f, lane.w - 2.0f),
                  std::max(0.0f, lane.h - 2.0f));
}

bool BuildMarker(const SDL_FRect& inner_lane, std::size_t total_rows, const MarkerInput& input,
                 Marker& out) {
  if (inner_lane.w <= 0.0f || inner_lane.h <= 0.0f || total_rows == 0 ||
      input.end_row <= input.start_row || input.start_row < 0) {
    return false;
  }

  const float total_units = static_cast<float>(total_rows);
  const float lane_end = inner_lane.y + inner_lane.h;
  const float top =
      inner_lane.y + (static_cast<float>(input.start_row) / total_units) * inner_lane.h;
  const float bottom =
      inner_lane.y + (static_cast<float>(input.end_row) / total_units) * inner_lane.h;
  float y = std::clamp(std::floor(top), inner_lane.y, std::max(inner_lane.y, lane_end - 1.0f));
  float height = std::max(kMinMarkerHeight, std::ceil(bottom) - y);
  if (y + height > lane_end) {
    y = std::max(inner_lane.y, lane_end - height);
    height = std::min(height, lane_end - y);
  }
  if (height <= 0.0f) {
    return false;
  }

  out = Marker{.rect = MakeRect(inner_lane.x, y, inner_lane.w, height), .color = input.color};
  return true;
}

std::vector<Marker> BuildMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                 std::span<const MarkerInput> inputs) {
  std::vector<Marker> markers;
  if (inner_lane.w <= 0.0f || inner_lane.h <= 0.0f || total_rows == 0 || inputs.empty()) {
    return markers;
  }

  markers.reserve(inputs.size());
  Marker marker;
  for (const MarkerInput& input : inputs) {
    if (BuildMarker(inner_lane, total_rows, input, marker)) {
      markers.push_back(marker);
    }
  }
  return markers;
}

std::vector<Marker> ReduceMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                  std::span<const MarkerInput> inputs,
                                  std::vector<std::uint32_t>& bucket_scratch,
                                  std::vector<SDL_Color>& palette_scratch) {
  std::vector<Marker> markers;
  if (inner_lane.w <= 0.0f || inner_lane.h <= 0.0f || total_rows == 0 || inputs.empty()) {
    return markers;
  }

  const int height_px = static_cast<int>(std::ceil(inner_lane.h));
  if (height_px <= 0) {
    return markers;
  }
  constexpr int kMinPx = static_cast<int>(kMinMarkerHeight);

  // 0 is the empty sentinel; each occupied bucket stores (packed_priority_color + 1).
  bucket_scratch.assign(static_cast<std::size_t>(height_px), 0u);
  palette_scratch.clear();

  auto color_index = [&](const SDL_Color& color) -> int {
    for (std::size_t i = 0; i < palette_scratch.size(); ++i) {
      if (SameColor(palette_scratch[i], color)) {
        return static_cast<int>(i);
      }
    }
    palette_scratch.push_back(color);
    return static_cast<int>(palette_scratch.size() - 1);
  };

  const float total_units = static_cast<float>(total_rows);
  for (const MarkerInput& input : inputs) {
    if (input.end_row <= input.start_row || input.start_row < 0) {
      continue;
    }
    const float top_px = (static_cast<float>(input.start_row) / total_units) * inner_lane.h;
    const float bottom_px = (static_cast<float>(input.end_row) / total_units) * inner_lane.h;
    int py0 = static_cast<int>(std::floor(top_px));
    int py1 = static_cast<int>(std::ceil(bottom_px));
    if (py1 < py0 + kMinPx) {
      py1 = py0 + kMinPx;  // keep a lone row visible
    }
    py0 = std::clamp(py0, 0, height_px - 1);
    py1 = std::clamp(py1, py0 + 1, height_px);
    if (py1 == height_px && py1 - py0 < kMinPx) {
      py0 = std::max(0, height_px - kMinPx);  // shift up so the min height still fits
    }

    const int ci = color_index(input.color);
    const std::uint32_t priority = static_cast<std::uint32_t>(std::max(0, input.priority));
    // Pack priority (for the winner comparison) above the color index (only for color
    // recovery in the coalescing pass). The winner is chosen on priority ALONE: a strictly
    // higher priority overwrites, and equal priorities keep the first writer. This makes a
    // contested pixel resolve by (priority, input order) deterministically — never by the
    // incidental palette insertion order that a full-packed-value compare would leak.
    const std::uint32_t stored =
        ((priority << 16) | static_cast<std::uint32_t>(ci)) + 1u;
    for (int py = py0; py < py1; ++py) {
      const std::uint32_t existing = bucket_scratch[static_cast<std::size_t>(py)];
      if (existing == 0u || priority > ((existing - 1u) >> 16)) {
        bucket_scratch[static_cast<std::size_t>(py)] = stored;
      }
    }
  }

  const float lane_end = inner_lane.y + inner_lane.h;
  int py = 0;
  while (py < height_px) {
    const std::uint32_t stored = bucket_scratch[static_cast<std::size_t>(py)];
    if (stored == 0u) {
      ++py;
      continue;
    }
    const int ci = static_cast<int>((stored - 1u) & 0xFFFFu);
    const int run_start = py;
    while (py < height_px) {
      const std::uint32_t next = bucket_scratch[static_cast<std::size_t>(py)];
      if (next == 0u || static_cast<int>((next - 1u) & 0xFFFFu) != ci) {
        break;
      }
      ++py;
    }
    const float y = inner_lane.y + static_cast<float>(run_start);
    const float height = std::min(static_cast<float>(py - run_start), std::max(0.0f, lane_end - y));
    if (height > 0.0f) {
      markers.push_back(Marker{.rect = MakeRect(inner_lane.x, y, inner_lane.w, height),
                               .color = palette_scratch[static_cast<std::size_t>(ci)]});
    }
  }
  return markers;
}

void DrawLane(SDL_Renderer* renderer, const render::Theme& theme, const SDL_FRect& lane,
              std::span<const Marker> markers) {
  if (renderer == nullptr || lane.w <= 0.0f || lane.h <= 0.0f) {
    return;
  }

  render::SetDrawColor(renderer, theme.surface_raised);
  SDL_RenderFillRect(renderer, &lane);
  render::SetDrawColor(renderer, theme.border);
  SDL_RenderRect(renderer, &lane);

  for (const Marker& marker : markers) {
    render::SetDrawColor(renderer, marker.color);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

std::uint64_t ThemeMarkerToken(const render::Theme& theme) {
  // FNV-1a over the colors that overview markers resolve from, across all three surfaces
  // (compare/merge diff colors, editor diagnostics + search, and the caret). The lane
  // chrome (surface_raised/border) is drawn live every frame so it needs no token, but
  // including the marker set here is enough to invalidate every baked-in color on a switch.
  const SDL_Color colors[] = {
      theme.diff_added,        theme.diff_deleted,       theme.diff_modified,
      theme.text_muted,        theme.accent,             theme.text_disabled,
      theme.diagnostic_error,  theme.diagnostic_warning, theme.diagnostic_info,
      theme.diagnostic_hint,   theme.search_match,       theme.search_match_active,
      theme.cursor,
  };
  std::uint64_t hash = 1469598103934665603ull;
  for (const SDL_Color& color : colors) {
    for (const std::uint8_t byte : {color.r, color.g, color.b, color.a}) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

}  // namespace microide::workspace::overview
