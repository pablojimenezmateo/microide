#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <SDL3/SDL.h>

#include "util/InlineVector.h"
#include "workspace/SplitTree.h"

namespace microide::workspace {

// Width of the grab band between two panes of a split, on every surface that
// lays panes out with a `SplitTree` (the editor grid and a terminal tab), so the
// two divider kinds feel identical under the pointer.
inline constexpr float kSplitDividerThickness = 6.0f;

// One divider between two adjacent panes of a split branch. `node`/`boundary`
// address it in the tree (stable across a resize, which is what a live drag
// needs); `pair_start`/`pair_extent` are the axis span the two panes SHARE, so a
// drag converts a pointer position into that pair's share without re-walking the
// tree or knowing where the branch sits.
struct SplitDividerRect {
  SDL_FRect rect{};
  std::uint8_t node = 0;
  std::uint8_t boundary = 0;
  bool vertical = true;  // true: separates side-by-side panes (drags along x)
  float pair_start = 0.0f;
  float pair_extent = 0.0f;

  // The share of the pair a pointer at (x, y) asks for, along this divider's axis.
  float ShareAt(float x, float y) const {
    return pair_extent > 0.0f ? ((vertical ? x : y) - pair_start) / pair_extent : 0.5f;
  }
};

// Every leaf's region carved out of an area, in leaf order, plus one divider per
// adjacent pair of every branch. Heap-free: rebuilt on hit-test, cursor and render
// paths (several times per mouse-motion event during a drag).
struct SplitRegionsLayout {
  util::InlineVector<SDL_FRect, kMaxSplitLeaves> panes;
  util::InlineVector<SplitDividerRect, kMaxSplitLeaves - 1> dividers;
};

// Lay `tree` out over `area`: branches split their rect along their axis by the
// per-child weights, with a `divider` band between neighbours; each child's extent
// is floored to whole pixels and the last child absorbs the remainder, so panes
// tile the area exactly. A single-leaf tree yields `area` itself and no divider.
SplitRegionsLayout ComputeSplitRegions(const SplitTree& tree,
                                       const SDL_FRect& area,
                                       float divider = kSplitDividerThickness);

// Sentinel: no region contains the point / lies that way.
inline constexpr std::size_t kNoSplitRegion = static_cast<std::size_t>(-1);

// Index of the region containing the point (dividers belong to none), or
// `kNoSplitRegion`.
std::size_t SplitRegionIndexAt(std::span<const SDL_FRect> regions, float x, float y);
inline std::size_t SplitRegionIndexAt(const SplitRegionsLayout& layout, float x, float y) {
  return SplitRegionIndexAt(std::span<const SDL_FRect>(layout.panes.data(), layout.panes.size()), x, y);
}

// Which way a directional pane verb points (VS Code's focusLeftGroup /
// moveActiveEditorGroupLeft family, and the terminal's pane-focus verbs).
enum class PaneDirection : std::uint8_t { Left, Right, Up, Down };

// The region adjacent to `from` in `direction`, by the geometry already computed:
// among the regions wholly on that side, the nearest one, tie-broken by how much
// of the cross axis it shares with `from` and then by centre distance. Pure: the
// answer is exactly what is on screen. `slack` absorbs the divider band and the
// rounding the tiling walk pushes into the last pane.
std::size_t AdjacentSplitRegion(std::span<const SDL_FRect> regions,
                                std::size_t from,
                                PaneDirection direction,
                                float slack = kSplitDividerThickness + 1.0f);

}  // namespace microide::workspace
