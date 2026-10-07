#pragma once

#include <cstddef>
#include <span>

#include <SDL3/SDL.h>

#include "util/InlineVector.h"

namespace microide::workspace {

// A terminal strip tab holds at most this many sessions side by side (VS Code's
// terminal group). Like `kMaxEditorGroups` this is the single definition of the
// cap: it is the capacity of the weights vector, of the pane-rect container the
// hit tests rebuild per event, and of the per-frame grid key, so raising it
// cannot leave one of them behind. Six fits an 80-column shell per pane on a
// 4K display in the maximized panel; past that a second tab is the better tool.
inline constexpr std::size_t kMaxTerminalPanes = 6;

// Width of the grab band between two panes. Same as the editor grid's so the
// two divider kinds feel identical under the pointer.
inline constexpr float kTerminalPaneDividerThickness = 6.0f;

// Smallest share of a pair either pane may be squeezed to. Keeps a pane from
// being dragged to nothing; matches the editor grid's clamp.
inline constexpr float kTerminalPaneMinShare = 0.1f;

// Per-pane horizontal shares of a tab's body, parallel to the tab's panes and
// normalised to sum to 1 after every edit.
using TerminalPaneWeights = util::InlineVector<float, kMaxTerminalPanes>;

// Rescale so the shares sum to 1; an empty or degenerate (all-zero) vector
// becomes an even split.
void NormalizeTerminalPaneWeights(TerminalPaneWeights& weights);

// A new pane at `index` takes half of the share of the pane it was split from
// (the one now at `index - 1`, or at `index` when inserting at the front), the
// way VS Code halves the split pane rather than squeezing every pane in the row.
// No-op when the row is full.
void InsertTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index);

// Drop pane `index`, giving its share to the pane on its left (the right
// neighbour when it was first). A single remaining pane is normalised to 1.
void RemoveTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index);

// Move the divider between `boundary` and `boundary + 1` to `first_share` of
// the two panes' COMBINED extent; only that pair moves. Returns false when the
// boundary does not name a live divider.
bool ResizeTerminalPaneDivider(TerminalPaneWeights& weights,
                               std::size_t boundary,
                               float first_share);

// Restore an even share between that pair (divider double-click).
bool ResetTerminalPaneDivider(TerminalPaneWeights& weights, std::size_t boundary);

// One divider between two adjacent panes. `pair_start`/`pair_extent` are the
// horizontal span the two panes SHARE, so a drag converts the pointer straight
// into that pair's share.
struct TerminalPaneDividerRect {
  SDL_FRect rect{};
  std::size_t boundary = 0;
  float pair_start = 0.0f;
  float pair_extent = 0.0f;
};

// Rects carved out of a tab's body rect, in pane order, plus one divider per
// adjacent pair. Heap-free: rebuilt on hit-test, cursor and render paths.
struct TerminalPaneRectsLayout {
  util::InlineVector<SDL_FRect, kMaxTerminalPanes> panes;
  util::InlineVector<TerminalPaneDividerRect, kMaxTerminalPanes - 1> dividers;
};

// Partition `body` horizontally by `weights`. Edges are floored to whole pixels
// and the last pane absorbs the rounding remainder, so adjacent panes never
// overlap and never leave a gap. One weight yields the body itself.
TerminalPaneRectsLayout ComputeTerminalPaneRects(const SDL_FRect& body,
                                                 std::span<const float> weights);

// Index of the pane whose rect contains the point (dividers belong to no pane),
// or `weights.size()` when none does.
std::size_t TerminalPaneIndexAt(const TerminalPaneRectsLayout& rects, float x, float y);

}  // namespace microide::workspace
