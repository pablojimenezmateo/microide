#include "workspace/coordinators/SelectionAutoscroll.h"

#include <algorithm>
#include <cstdint>

#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"

namespace microide::workspace::selection_autoscroll {

namespace {

constexpr int kMaxRowsPerTick = 12;
constexpr int kMaxColumnsPerTick = 24;

// +1 so any overshoot at all moves by one unit; the rest scales with distance.
int StepFor(float overshoot, float unit, int cap) {
  if (overshoot <= 0.0f) {
    return 0;
  }
  return std::min(cap, 1 + static_cast<int>(overshoot / std::max(1.0f, unit)));
}

}  // namespace

void Arm(InteractionState& interaction_state,
         const SDL_FRect& editor_rect,
         const editor::EditorViewMetrics& metrics,
         float pointer_x,
         float pointer_y) {
  const float line_height = std::max(1.0f, metrics.line_height);
  const float band_top = metrics.first_line_y;
  const float band_bottom =
      band_top + static_cast<float>(std::max<std::size_t>(1, metrics.visible_rows)) * line_height;

  interaction_state.selection_autoscroll_rows =
      -StepFor(band_top - pointer_y, line_height, kMaxRowsPerTick) +
      StepFor(pointer_y - band_bottom, line_height, kMaxRowsPerTick);
  // The horizontal ramp uses the line height as its unit too. Only the ramp rate
  // depends on it, and a column is far narrower than a row is tall, so the same
  // number of pixels past the edge buys proportionally more columns than rows --
  // which is the right feel, since horizontal overshoot is usually deliberate.
  interaction_state.selection_autoscroll_columns =
      -StepFor(editor_rect.x - pointer_x, line_height, kMaxColumnsPerTick) +
      StepFor(pointer_x - (editor_rect.x + editor_rect.w), line_height, kMaxColumnsPerTick);
  interaction_state.selection_pointer_x = pointer_x;
  interaction_state.selection_pointer_y = pointer_y;
}

void Disarm(InteractionState& interaction_state) {
  interaction_state.selection_autoscroll_rows = 0;
  interaction_state.selection_autoscroll_columns = 0;
}

std::optional<Uint32> NextDelayMs(const InteractionState& interaction_state) {
  if (!interaction_state.selection_autoscroll_active()) {
    return std::nullopt;
  }
  return static_cast<Uint32>(40);  // ~25 steps/sec
}

bool Step(InteractionState& interaction_state, editor::TextViewport& viewport) {
  if (!interaction_state.selection_autoscroll_active()) {
    return false;
  }
  // Belt and braces against a stuck autoscroll. A button-up can go missing --
  // released over another window, or the compositor takes the grab -- and this is
  // the one loop in the shell that keeps running with no input behind it, so a
  // velocity left armed would scroll forever with no button down. An unfocused
  // window is not dragging.
  //
  // Deliberately shell state rather than SDL_GetMouseState: the live pointer is
  // not the authority on a gesture tracked through events (it is not updated by
  // synthetic events at all, so it would also make this untestable), and the
  // event dispatcher already ends the gesture on focus loss.
  if (!interaction_state.window_has_input_focus) {
    interaction_state.mouse_selecting = false;
    Disarm(interaction_state);
    return false;
  }

  const std::size_t line_count = viewport.line_count();
  const std::size_t before_scroll = viewport.scroll_line();
  const std::size_t before_horizontal = viewport.horizontal_scroll();
  if (const int rows = interaction_state.selection_autoscroll_rows; rows != 0) {
    const std::int64_t target = static_cast<std::int64_t>(before_scroll) + rows;
    const std::int64_t max_scroll =
        static_cast<std::int64_t>(line_count == 0 ? 0 : line_count - 1);
    viewport.SetScrollLine(
        static_cast<std::size_t>(std::clamp<std::int64_t>(target, 0, max_scroll)));
  }
  if (const int columns = interaction_state.selection_autoscroll_columns; columns != 0) {
    const std::int64_t target = static_cast<std::int64_t>(before_horizontal) + columns;
    viewport.SetHorizontalScroll(static_cast<std::size_t>(std::max<std::int64_t>(0, target)));
  }
  if (viewport.scroll_line() == before_scroll &&
      viewport.horizontal_scroll() == before_horizontal) {
    // Already at the end the pointer is pushing toward. Stop asking for wakes;
    // a later motion event re-arms if the pointer moves to the other edge.
    Disarm(interaction_state);
    return false;
  }
  return true;
}

}  // namespace microide::workspace::selection_autoscroll
