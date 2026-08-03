#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace microide::workspace {

// Minimal view of a visible tab needed to animate a strip. Callers build these
// from their `VisibleStripTab` rects so this math module stays free of the heavy
// workspace-state headers and is trivially unit-testable.
struct SlideTab {
  std::size_t index = 0;  // model-space index (matches VisibleStripTab::index)
  float x = 0.0f;         // base rect.x (resting layout, no animation applied)
  float width = 0.0f;     // base rect.w
};

// Chrome-like reorder layout: returns the target x for each input tab (same
// order) assuming the dragged tab (`source_index`) is lifted out of the flow and
// a gap of `ghost_width + gap` opens before the first tab whose index is
// `>= insertion_slot`. The dragged tab's own entry is returned as its input x
// (callers ignore it — it renders as the floating ghost). Handles variable tab
// widths and dropping past the last tab (gap trails the strip). Tabs must be in
// ascending index order, as produced by the strip layout.
std::vector<float> ComputeSlideTargetXs(std::span<const SlideTab> tabs,
                                        std::size_t source_index,
                                        std::size_t insertion_slot,
                                        float ghost_width,
                                        float gap);

// Frame-rate-independent exponential smoothing of `current` toward `target`.
// `dt_ms` is the elapsed time since the last advance. Any offset within a small
// pixel threshold of its target snaps exactly to it. Returns true if any offset
// is still in motion (i.e. another frame is needed). `current` and `target` must
// be the same size; mismatched sizes are treated as "not animating".
bool AdvanceSlideOffsets(std::vector<float>& current,
                         const std::vector<float>& target,
                         float dt_ms);

// True if any paired offset differs by more than the snap threshold, i.e. the
// strip would visibly move on the next advance.
bool SlideOffsetsMoving(const std::vector<float>& current,
                        const std::vector<float>& target);

}  // namespace microide::workspace
