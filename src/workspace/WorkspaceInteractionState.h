#pragma once

#include <SDL3/SDL.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

enum class DragTarget {
  None,
  SidebarDivider,
  BottomPanelDivider,
  CompareDivider,
  MergeLeftDivider,
  MergeRightDivider,
  SidebarScrollbar,
  BottomPanelScrollbar,
  OverlayScrollbar,
  EditorVerticalScrollbar,
  EditorHorizontalScrollbar,
  EditorSplitDivider,
  CompareVerticalScrollbar,
  CompareHorizontalScrollbar,
  SingleLineSelection,
};

enum class TabDragKind {
  None,
  Project,
  Editor,
  Terminal,
};

struct TabDragState {
  TabDragKind kind = TabDragKind::None;
  float press_x = 0.0f;
  float press_y = 0.0f;
  bool dragging = false;
  bool reordered = false;
};

struct InteractionState {
  bool window_has_input_focus = true;
  bool mouse_selecting = false;
  DragTarget drag_target = DragTarget::None;
  float drag_scrollbar_offset = 0.0f;
  std::vector<std::size_t> drag_editor_split_path;
  std::size_t drag_editor_split_divider_index = 0;
  TabDragState tab_drag;
  Uint64 last_title_bar_click_ms = 0;
  float last_title_bar_click_x = 0.0f;
  float last_title_bar_click_y = 0.0f;
  // Sub-tick wheel accumulators. High-resolution trackpads and touchpads emit
  // many SDL_EVENT_MOUSE_WHEEL events with fractional `y`/`x` deltas. The
  // legacy path used `event.wheel.integer_y` which rounds those to zero, so
  // smooth-scroll input produced a stair-step of "no scroll, no scroll, then a
  // 3-line jump". We accumulate the float deltas across events and only emit
  // whole-tick scrolls when |accumulator| >= 1.
  float wheel_accumulator_y = 0.0f;
  float wheel_accumulator_x = 0.0f;
  // Identifies which single-line input owns the in-flight drag-select gesture so the drag
  // handler can keep updating the right editor without a second hit-test.
  TextInputSurface single_line_drag_surface = TextInputSurface::None;
};

struct WheelTicks {
  int vertical = 0;
  int horizontal = 0;
};

// Pure helper extracted from WorkspaceShell::HandleMouseWheel so the
// accumulator behavior can be unit-tested independently of the full event
// dispatch path. Folds the raw SDL wheel deltas into whole-line ticks using
// the persistent `wheel_accumulator_{x,y}` state on `interaction`. Mutates
// the accumulators in place and returns the ticks that should be dispatched
// to downstream coordinators this event. `flipped` mirrors
// `event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED`. Non-finite inputs are
// dropped defensively so a stray NaN cannot poison the accumulator.
inline WheelTicks AccumulateWheelEvent(InteractionState& interaction,
                                       float raw_y,
                                       float raw_x,
                                       bool flipped) {
  const float flip_sign = flipped ? -1.0f : 1.0f;
  const float dy = std::isfinite(raw_y) ? raw_y * flip_sign : 0.0f;
  const float dx = std::isfinite(raw_x) ? raw_x * flip_sign : 0.0f;
  float& accum_y = interaction.wheel_accumulator_y;
  float& accum_x = interaction.wheel_accumulator_x;
  if ((dy > 0.0f && accum_y < 0.0f) || (dy < 0.0f && accum_y > 0.0f)) {
    accum_y = 0.0f;
  }
  if ((dx > 0.0f && accum_x < 0.0f) || (dx < 0.0f && accum_x > 0.0f)) {
    accum_x = 0.0f;
  }
  accum_y += dy;
  accum_x += dx;
  const int vertical = static_cast<int>(std::trunc(accum_y));
  const int horizontal = static_cast<int>(std::trunc(accum_x));
  accum_y -= static_cast<float>(vertical);
  accum_x -= static_cast<float>(horizontal);
  return WheelTicks{.vertical = vertical, .horizontal = horizontal};
}

}  // namespace microide::workspace
