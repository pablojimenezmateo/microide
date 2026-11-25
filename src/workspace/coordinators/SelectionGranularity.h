#pragma once

#include "editor/EditTypes.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace::selection_granularity {

void RecordSeed(InteractionState& interaction_state,
                InteractionState::SelectionGranularity granularity,
                const editor::TextViewport& viewport);

void ApplyClick(InteractionState& interaction_state,
                editor::TextViewport& viewport,
                int clicks);

void ExtendToPointer(const InteractionState& interaction_state,
                     editor::TextViewport& viewport,
                     editor::TextPosition hit_position);

[[nodiscard]] inline bool DragIsGranular(const InteractionState& interaction_state) {
  return interaction_state.selection_granularity !=
         InteractionState::SelectionGranularity::Character;
}

}
