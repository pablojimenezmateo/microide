#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::editor {
class TextViewport;
struct EditorViewMetrics;
}

namespace microide::workspace {
struct TextGridInteractionLayout;
}

namespace microide::workspace::selection_autoscroll {

struct Band {
  SDL_FRect rect{};
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  std::size_t visible_rows = 1;
};

Band BandFor(const SDL_FRect& pane_rect, const editor::EditorViewMetrics& metrics);
Band BandFor(const TextGridInteractionLayout& interaction);

void Arm(InteractionState& interaction_state,
         InteractionState::SelectionAutoscrollOwner owner,
         const Band& band,
         float pointer_x,
         float pointer_y);

SDL_FPoint ClampPointerToBand(const Band& band, float x, float y);

void Disarm(InteractionState& interaction_state);

std::optional<Uint32> NextDelayMs(const InteractionState& interaction_state);

struct StepDelta {
  int rows = 0;
  int columns = 0;
};

std::optional<StepDelta> BeginStep(InteractionState& interaction_state);

bool FinishStep(InteractionState& interaction_state, bool moved);

bool Step(InteractionState& interaction_state, editor::TextViewport& viewport);

}
