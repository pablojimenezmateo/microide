#include "workspace/coordinators/SelectionAutoscroll.h"

#include <algorithm>
#include <cstdint>

#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace::selection_autoscroll {

namespace {

constexpr int kMaxRowsPerTick = 12;
constexpr int kMaxColumnsPerTick = 24;

int StepFor(float overshoot, float unit, int cap) {
  if (overshoot <= 0.0f) {
    return 0;
  }
  return std::min(cap, 1 + static_cast<int>(overshoot / std::max(1.0f, unit)));
}

}

Band BandFor(const SDL_FRect& pane_rect, const editor::EditorViewMetrics& metrics) {
  return Band{
      .rect = pane_rect,
      .first_line_y = metrics.first_line_y,
      .line_height = metrics.line_height,
      .visible_rows = metrics.visible_rows,
  };
}

Band BandFor(const TextGridInteractionLayout& interaction) {
  return Band{
      .rect = interaction.rect,
      .first_line_y = interaction.first_line_y,
      .line_height = interaction.line_height,
      .visible_rows = interaction.visible_rows,
  };
}

SDL_FPoint ClampPointerToBand(const Band& band, float x, float y) {
  const float line_height = std::max(1.0f, band.line_height);
  const float band_top = band.first_line_y;
  const float band_bottom =
      band_top + static_cast<float>(std::max<std::size_t>(1, band.visible_rows)) * line_height;
  return SDL_FPoint{
      .x = std::clamp(x, band.rect.x, band.rect.x + std::max(0.0f, band.rect.w - 1.0f)),
      .y = std::clamp(y, band_top, std::max(band_top, band_bottom - line_height * 0.5f)),
  };
}

void Arm(InteractionState& interaction_state,
         InteractionState::SelectionAutoscrollOwner owner,
         const Band& band,
         float pointer_x,
         float pointer_y) {
  const float line_height = std::max(1.0f, band.line_height);
  const float band_top = band.first_line_y;
  const float band_bottom =
      band_top + static_cast<float>(std::max<std::size_t>(1, band.visible_rows)) * line_height;

  interaction_state.selection_autoscroll_rows =
      -StepFor(band_top - pointer_y, line_height, kMaxRowsPerTick) +
      StepFor(pointer_y - band_bottom, line_height, kMaxRowsPerTick);
  interaction_state.selection_autoscroll_columns =
      -StepFor(band.rect.x - pointer_x, line_height, kMaxColumnsPerTick) +
      StepFor(pointer_x - (band.rect.x + band.rect.w), line_height, kMaxColumnsPerTick);
  interaction_state.selection_pointer_x = pointer_x;
  interaction_state.selection_pointer_y = pointer_y;
  interaction_state.selection_autoscroll_owner =
      interaction_state.selection_autoscroll_rows != 0 || interaction_state.selection_autoscroll_columns != 0
          ? owner
          : InteractionState::SelectionAutoscrollOwner::None;
}

void Disarm(InteractionState& interaction_state) {
  interaction_state.selection_autoscroll_rows = 0;
  interaction_state.selection_autoscroll_columns = 0;
  interaction_state.selection_autoscroll_owner = InteractionState::SelectionAutoscrollOwner::None;
}

std::optional<Uint32> NextDelayMs(const InteractionState& interaction_state) {
  if (!interaction_state.selection_autoscroll_active()) {
    return std::nullopt;
  }
  return static_cast<Uint32>(40);
}

std::optional<StepDelta> BeginStep(InteractionState& interaction_state) {
  if (!interaction_state.selection_autoscroll_active()) {
    return std::nullopt;
  }
  if (!interaction_state.window_has_input_focus) {
    interaction_state.mouse_selecting = false;
    Disarm(interaction_state);
    return std::nullopt;
  }
  return StepDelta{
      .rows = interaction_state.selection_autoscroll_rows,
      .columns = interaction_state.selection_autoscroll_columns,
  };
}

bool FinishStep(InteractionState& interaction_state, bool moved) {
  if (!moved) {
    Disarm(interaction_state);
    return false;
  }
  return true;
}

bool Step(InteractionState& interaction_state, editor::TextViewport& viewport) {
  const std::optional<StepDelta> delta = BeginStep(interaction_state);
  if (!delta.has_value()) {
    return false;
  }

  const std::size_t line_count = viewport.visual_line_count();
  const std::size_t before_scroll = viewport.scroll_line();
  const std::size_t before_horizontal = viewport.horizontal_scroll();
  if (delta->rows != 0) {
    const std::int64_t target = static_cast<std::int64_t>(before_scroll) + delta->rows;
    const std::int64_t max_scroll =
        static_cast<std::int64_t>(line_count == 0 ? 0 : line_count - 1);
    viewport.SetScrollLine(
        static_cast<std::size_t>(std::clamp<std::int64_t>(target, 0, max_scroll)));
  }
  if (delta->columns != 0) {
    const std::int64_t target = static_cast<std::int64_t>(before_horizontal) + delta->columns;
    viewport.SetHorizontalScroll(static_cast<std::size_t>(std::max<std::int64_t>(0, target)));
  }
  return FinishStep(interaction_state, viewport.scroll_line() != before_scroll ||
                                           viewport.horizontal_scroll() != before_horizontal);
}

}
