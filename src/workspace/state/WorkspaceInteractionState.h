#pragma once

#include <SDL3/SDL.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

enum class DragTarget {
  None,
  SidebarDivider,
  RightPaneDivider,
  BottomPanelDivider,
  CompareDivider,
  MergeLeftDivider,
  MergeRightDivider,
  SidebarScrollbar,
  BottomPanelScrollbar,
  DebugPaneScrollbar,
  OverlayScrollbar,
  EditorVerticalScrollbar,
  EditorHorizontalScrollbar,
  EditorSplitDivider,
  CompareVerticalScrollbar,
  CompareHorizontalScrollbar,
  SettingsScrollbar,
  SettingsCategoryScrollbar,
  SettingsPickerScrollbar,
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
  // True once the live pointer would commit to a slot other than the source.
  // Decides whether mouse-up persists the reorder.
  bool reordered = false;
  // Deferred-commit drag state. The dragged tab is NOT moved in the model while
  // dragging; instead we track the pointer and a computed target slot, render a
  // floating ghost + insertion caret, and commit a single reorder on release.
  std::size_t source_index = 0;     // active index captured at press
  float pointer_x = 0.0f;           // live cursor x (drives ghost + slot)
  float pointer_y = 0.0f;
  std::size_t target_slot = 0;      // StripInsertionSlot() for pointer_x
  float ghost_width = 0.0f;         // dragged tab rect width
  float grab_offset_x = 0.0f;       // press_x - dragged_tab.rect.x
};

// Chrome-like sliding reorder animation. While a tab drag is live, neighbor tabs
// ease from their base x toward a "displaced" layout (dragged tab lifted out, a
// gap opened at the insertion slot); on release the dropped tab glides from the
// ghost position into its committed slot. Offsets are indexed by model tab index
// and only the visible subset is filled. This outlives `tab_drag` so the
// post-release settle can finish (the drag state clears on button-up but the
// glide keeps running until every offset reaches 0).
struct TabSlideState {
  TabDragKind kind = TabDragKind::None;  // which strip animates; None = idle
  std::size_t group_index = 0;           // editor group when kind == Editor
  std::vector<float> current;            // per model-index x offset (rendered)
  std::vector<float> target;             // per model-index x offset (goal)
  Uint64 last_advance_ms = 0;            // SDL_GetTicks() of last ease step
  bool settling = false;                 // post-release glide in progress
};

struct InteractionState {
  bool window_has_input_focus = true;
  bool mouse_selecting = false;
  DragTarget drag_target = DragTarget::None;
  float drag_scrollbar_offset = 0.0f;
  std::size_t drag_editor_split_divider_index = 0;
  TabDragState tab_drag;
  TabSlideState tab_slide;
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
  // Editor column/box selection drag (Shift+Alt+drag). While active, editor
  // selection-motion extends a rectangular per-line selection from the fixed
  // anchor corner to the pointer instead of moving the single primary caret.
  // The anchor is the primary caret position captured at press. Stored as raw
  // line/column so this header keeps no editor-type dependency.
  bool editor_box_selecting = false;
  std::size_t editor_box_anchor_line = 0;
  std::size_t editor_box_anchor_column = 0;
  // Selection-drag autoscroll. A drag whose pointer is held past an edge of the
  // visible text band clamps onto the edge cell, so without this the selection
  // stops growing at the first/last visible row -- you cannot select more than a
  // screenful by dragging, which is what every other editor does.
  //
  // The pointer position is stored because the scroll is driven by the idle
  // wake, not by motion events: the pointer is by definition NOT moving while it
  // is held outside, so there is no event to carry it. Signed rows/columns per
  // tick, zero when the pointer is inside the band (which is what disarms the
  // wake). Cleared on button-up alongside `mouse_selecting`.
  int selection_autoscroll_rows = 0;
  int selection_autoscroll_columns = 0;
  // Which gesture armed the deltas above. `mouse_selecting` cannot answer this:
  // the terminal panel tracks its drag on its own tab state (a shell-level
  // mouse_selecting there would also extend the active editor's selection on
  // every motion event), so an autoscroll keyed on it could never reach the
  // terminal (TD-2026-08-13-205). selection_autoscroll::Arm/Disarm own this
  // flag, and every button-up, focus loss and project switch disarms.
  bool selection_autoscroll_armed = false;
  float selection_pointer_x = 0.0f;
  float selection_pointer_y = 0.0f;
  // Granularity of the in-flight selection drag. A double-click that then drags
  // selects whole WORDS and a triple-click drag whole LINES, in every editor --
  // the click's own expansion used to be collapsed back to character granularity
  // by the first motion event, because the drag extended from the caret with no
  // memory of how the gesture started.
  //
  // `selection_seed` is the range the initiating click expanded to. The drag
  // never shrinks below it: the selection is always the union of the seed and the
  // word/line under the pointer, which is what makes dragging back across the
  // seed feel right instead of inverting inside it.
  enum class SelectionGranularity : std::uint8_t { Character, Word, Line };
  SelectionGranularity selection_granularity = SelectionGranularity::Character;
  std::size_t selection_seed_start_line = 0;
  std::size_t selection_seed_start_column = 0;
  std::size_t selection_seed_end_line = 0;
  std::size_t selection_seed_end_column = 0;
  bool selection_autoscroll_active() const {
    return selection_autoscroll_armed &&
           (selection_autoscroll_rows != 0 || selection_autoscroll_columns != 0);
  }
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
