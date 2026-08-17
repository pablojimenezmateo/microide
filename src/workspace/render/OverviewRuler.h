#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace microide::render {
struct Theme;
}  // namespace microide::render

// Shared "overview ruler" primitive: the thin marker lane painted immediately left
// of a vertical scrollbar track. It shows, at a glance, where notable rows live
// across the whole document (diff/merge changes, and — in the main editor — search
// matches, diagnostics, and the caret). This module owns the geometry, the row->pixel
// mapping, an optional density reducer for dense sources, and the draw primitive, so
// the compare, merge, and editor render paths share one implementation instead of
// three copies. It is deterministic and unit-testable: no shell access, no SDL event
// glue, colors are resolved by callers and passed in.
namespace microide::workspace::overview {

// Lane bar geometry, relative to the vertical scrollbar track it decorates.
inline constexpr float kLaneWidth = 6.0f;
inline constexpr float kLaneGap = 3.0f;
// Single-row markers paint at least this tall so a lone match stays visible.
inline constexpr float kMinMarkerHeight = 2.0f;

// One row interval [start_row, end_row) with a pre-resolved fill color. `priority`
// only matters for the density reducer: when two inputs map onto the same pixel row,
// the higher priority wins the pixel.
struct MarkerInput {
  int start_row = 0;
  int end_row = 0;  // exclusive
  SDL_Color color{};
  int priority = 0;
};

// A finished lane marker in absolute pixel coordinates.
struct Marker {
  SDL_FRect rect{};
  SDL_Color color{};
};

// Outer lane rect (background + border) left-adjacent to `track`, same y/height. The
// lane's left edge is clamped to `min_x` so a narrow surface never paints the lane over
// the split divider or the adjacent editor column to its left (callers pass the owning
// surface's left edge; the editor/default leaves it unclamped).
SDL_FRect LaneRect(const SDL_FRect& track,
                   float min_x = -std::numeric_limits<float>::infinity());
// Inner rect the markers paint into (1px inset of the lane for the border).
SDL_FRect LaneInnerRect(const SDL_FRect& lane);

// Maps one input's row interval onto `inner_lane`, writing the marker to `out` and
// returning true when a visible marker was produced (false for a degenerate/empty
// interval). Allocation-free: the live caret path draws through this so a steady frame
// never touches the heap.
bool BuildMarker(const SDL_FRect& inner_lane, std::size_t total_rows, const MarkerInput& input,
                 Marker& out);

// Maps each input's row interval onto `inner_lane` by linear proportion of
// `total_rows`, enforcing a >= kMinMarkerHeight height and clamping to the lane.
// Emits one marker per input in input order — intended for sparse, non-overlapping
// sources (diff runs, merge conflicts). For dense/overlapping sources use ReduceMarkers.
std::vector<Marker> BuildMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                 std::span<const MarkerInput> inputs);
// Into-form for the cached lanes. `out` is cleared and refilled, keeping its
// capacity: every caller assigns the result into a cache that is invalidated by
// a revision bump, so the returning form freed the marker buffer and allocated
// the same size straight back on the next frame that touched the surface
// (TD-2026-08-17-261).
void BuildMarkersInto(const SDL_FRect& inner_lane, std::size_t total_rows,
                      std::span<const MarkerInput> inputs, std::vector<Marker>& out);

// Density-bounded variant for dense/overlapping sources (thousands of search matches).
// Buckets every input's mapped pixel span into a per-pixel-row array (highest priority
// wins each contested pixel), then coalesces adjacent equal-color pixels into runs.
// Output size and cost are bounded by lane height, never by |inputs|. `bucket_scratch`
// and `palette_scratch` are caller-owned to avoid per-call allocation.
void ReduceMarkersInto(const SDL_FRect& inner_lane, std::size_t total_rows,
                       std::span<const MarkerInput> inputs,
                       std::vector<std::uint32_t>& bucket_scratch,
                       std::vector<SDL_Color>& palette_scratch, std::vector<Marker>& out);
std::vector<Marker> ReduceMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                  std::span<const MarkerInput> inputs,
                                  std::vector<std::uint32_t>& bucket_scratch,
                                  std::vector<SDL_Color>& palette_scratch);

// Draws the lane chrome (raised background + border) into `lane`, then the markers.
// `lane` is the outer rect from LaneRect (already clamped by the caller so the chrome and
// the markers share one geometry); `markers` are absolute (from BuildMarker(s)/ReduceMarkers
// on LaneInnerRect(lane)).
void DrawLane(SDL_Renderer* renderer, const render::Theme& theme, const SDL_FRect& lane,
              std::span<const Marker> markers);

// A cheap fingerprint of the theme colors that overview markers bake in. Marker caches key
// on revision/geometry, which a live colorscheme switch never bumps; folding this token
// into the key invalidates baked-in colors when (and only when) the theme changes.
std::uint64_t ThemeMarkerToken(const render::Theme& theme);

}  // namespace microide::workspace::overview
