#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
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

// Where an editor tab dragged over an editor PANE BODY (rather than over a tab
// strip) would land, VS Code style. `Center` moves it into that pane's group;
// the four edge zones carve a new group out of that pane on that side. `None`
// means the pointer is not offering a body drop at all -- it is over a strip, or
// over a pane where the drop would be a no-op.
enum class EditorBodyDropZone : std::uint8_t {
  None,
  Center,
  Left,
  Right,
  Top,
  Bottom,
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
  std::size_t target_slot = 0;      // insertion slot under the dragged tab's box
  float ghost_width = 0.0f;         // dragged tab rect width
  float grab_offset_x = 0.0f;       // press_x - dragged_tab.rect.x
  // Editor drags only. The group the press landed in (`source_index` indexes ITS
  // tab list) and the group whose strip the pointer is over right now. They differ
  // while the pointer is over the other half of a split, and a release in that
  // state moves the tab BETWEEN groups instead of reordering within one
  // (TD-2026-08-14-213). The source group keeps keyboard focus for the whole
  // gesture, so `source_group_index` stays the focused group and the commit guard
  // can still read the dragged tab as its group's active tab.
  std::size_t source_group_index = 0;
  std::size_t target_group_index = 0;
  bool cross_group() const { return kind == TabDragKind::Editor &&
                                    target_group_index != source_group_index; }
  // True once the slide targets have been seeded at least once for this drag, so
  // a motion event that does not change the insertion slot can advance the ease
  // instead of rebuilding the whole target layout.
  bool slide_seeded = false;
  // Drag auto-scroll of an overflowing strip (VS Code holds the pointer at the
  // edge and the strip walks under it). Direction is latched so the repeat can be
  // pumped from the animation tick while the pointer sits perfectly still, and
  // rate-limited so it steps at a readable pace rather than per motion event.
  int autoscroll_direction = 0;
  Uint64 last_autoscroll_ms = 0;
  // Drop-on-editor-body target (VS Code's drag-to-split). Resolved on every
  // motion event while the pointer sits over a pane body instead of a strip; the
  // commit and the translucent drop overlay both read it, and `body_drop_rect`
  // is the exact region that overlay paints, kept here so neither the renderer
  // nor the damage pass has to recompute the pane geometry.
  EditorBodyDropZone body_drop_zone = EditorBodyDropZone::None;
  std::size_t body_drop_group_index = 0;
  SDL_FRect body_drop_rect{0.0f, 0.0f, 0.0f, 0.0f};
  bool body_drop() const {
    return kind == TabDragKind::Editor && body_drop_zone != EditorBodyDropZone::None;
  }
};

// One animating tab strip's offsets. Indexed by model tab index; only the
// visible subset is ever filled, and an idle slot holds no vector capacity at
// all, so a same-group drag pays nothing for the second slot.
struct TabStripSlide {
  TabDragKind kind = TabDragKind::None;  // which strip animates; None = idle
  std::size_t group_index = 0;           // editor group when kind == Editor
  std::vector<float> current;            // per model-index x offset (rendered)
  std::vector<float> target;             // per model-index x offset (goal)

  bool idle() const { return kind == TabDragKind::None; }
  bool Is(TabDragKind for_kind, std::size_t for_group) const {
    return kind == for_kind && (kind != TabDragKind::Editor || group_index == for_group);
  }
};

// Two, because an editor tab dragged across a split animates BOTH strips at once
// — the source closes the hole the lifted tab left while the destination opens a
// gap at the insertion slot — and `kMaxEditorGroups` (WorkspaceLayout.h) caps a
// split at two groups. Spelled here rather than included so this header keeps no
// layout dependency.
inline constexpr std::size_t kMaxAnimatingTabStrips = 2;

// Chrome-like sliding reorder animation. While a tab drag is live, neighbor tabs
// ease from their base x toward a "displaced" layout (dragged tab lifted out, a
// gap opened at the insertion slot); on release the dropped tab glides from the
// ghost position into its committed slot. This outlives `tab_drag` so the
// post-release settle can finish (the drag state clears on button-up but the
// glide keeps running until every offset reaches 0).
//
// `strips[0]` is the strip the gesture started on and the only one a same-group
// drag or a project/terminal drag ever uses; `strips[1]` is armed only while an
// editor drag hovers the other group's strip (TD-2026-08-14-213).
struct TabSlideState {
  std::array<TabStripSlide, kMaxAnimatingTabStrips> strips;
  Uint64 last_advance_ms = 0;            // SDL_GetTicks() of last ease step
  bool settling = false;                 // post-release glide in progress

  bool active() const {
    for (const TabStripSlide& strip : strips) {
      if (!strip.idle()) {
        return true;
      }
    }
    return false;
  }
  // The rendered offsets for one strip, or an empty span when it is not
  // animating. Render paths index into this rather than reaching for a field, so
  // the two-strip case needs no branch of their own.
  std::span<const float> OffsetsFor(TabDragKind kind, std::size_t group_index) const {
    for (const TabStripSlide& strip : strips) {
      if (!strip.idle() && strip.Is(kind, group_index)) {
        return strip.current;
      }
    }
    return {};
  }
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
  // Dragging a selection to move it (VS Code's `editor.dragAndDrop`). A press
  // INSIDE an existing selection is ambiguous: it is either a click that
  // collapses the caret there — which is what it has always done, and must keep
  // doing — or the start of a move. So the press commits to neither until the
  // pointer travels past a threshold, which is why this is a third state rather
  // than another bool next to `mouse_selecting` (TD-2026-08-13-204).
  //
  // Pending: pressed inside a selection, undecided.
  // Dragging: past the threshold; the drop indicator is live and release moves
  // the text (copies it, with Ctrl held).
  enum class TextDragState : std::uint8_t { None, Pending, Dragging };
  TextDragState text_drag = TextDragState::None;
  float text_drag_press_x = 0.0f;
  float text_drag_press_y = 0.0f;
  // Where the press landed in the document, resolved at press time. The release
  // path needs it to perform the click the press deferred, and re-hit-testing
  // there would need a layout it does not have.
  std::size_t text_drag_press_line = 0;
  std::size_t text_drag_press_column = 0;
  // The selection captured at press. The edit reads its text from THIS range at
  // release rather than from the live selection, so a drop cannot move whatever
  // happens to be selected at the time.
  std::size_t text_drag_source_start_line = 0;
  std::size_t text_drag_source_start_column = 0;
  std::size_t text_drag_source_end_line = 0;
  std::size_t text_drag_source_end_column = 0;
  // Live drop point while Dragging; drives the insertion-point indicator.
  std::size_t text_drag_drop_line = 0;
  std::size_t text_drag_drop_column = 0;
  bool text_drag_has_drop = false;
  bool text_dragging() const { return text_drag == TextDragState::Dragging; }
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
