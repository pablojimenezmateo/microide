#pragma once

#include "editor/EditTypes.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::editor {
class TextViewport;
}  // namespace microide::editor

// Word- and line-granular drag selection.
//
// A double-click drags by whole words and a triple-click by whole lines, and
// neither may ever shrink below the unit the initiating click selected. Both
// halves -- recording what the click expanded to, and growing a drag to cover
// the unit under the pointer -- live here because three surfaces run three
// button-down paths (the editor pane, the compare right pane, the merge result
// pane) and the behaviour must not be a property of which one you clicked in.
namespace microide::workspace::selection_granularity {

// Remember what the initiating double/triple click expanded to, so the drag that
// follows can keep that granularity and never shrink below it.
void RecordSeed(InteractionState& interaction_state,
                InteractionState::SelectionGranularity granularity,
                const editor::TextViewport& viewport);

// Apply a click's granularity to `viewport`, seeding the drag that follows.
// `clicks` is SDL's click count: 1 leaves character granularity, 2 selects the
// word under the caret, 3 or more the line. The caret must already be placed.
void ApplyClick(InteractionState& interaction_state,
                editor::TextViewport& viewport,
                int clicks);

// Extend a word- or line-granular drag to cover the whole word/line under the
// pointer, unioned with the seed the initiating click established.
//
// The caret goes on whichever end moved away from the seed, so the selection
// grows in the direction of travel and dragging back across the seed flips the
// active end rather than eating into it.
void ExtendToPointer(const InteractionState& interaction_state,
                     editor::TextViewport& viewport,
                     editor::TextPosition hit_position);

// Whether a drag in progress should go through `ExtendToPointer` rather than a
// plain character-granular caret move.
[[nodiscard]] inline bool DragIsGranular(const InteractionState& interaction_state) {
  return interaction_state.selection_granularity !=
         InteractionState::SelectionGranularity::Character;
}

}  // namespace microide::workspace::selection_granularity
