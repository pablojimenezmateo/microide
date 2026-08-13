#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::editor {
class TextViewport;
struct EditorViewMetrics;
}  // namespace microide::editor

// Selection-drag autoscroll.
//
// A drag whose pointer is held past an edge of the visible text band clamps onto
// the edge cell (see EditorMouseCoordinator::HandleSelectionMotion), so without
// this the selection stops growing at the first/last visible row and a drag can
// never select more than a screenful.
//
// It lives here rather than on WorkspaceShell because none of it needs the
// shell: arming reads a pointer against a pane's metrics, stepping moves a
// viewport, and disarming clears two ints. The shell keeps only the two lines
// that connect it to the idle wake.
//
// The wake, not the motion handler, is what repeats the step: a pointer held
// still outside the band produces no further motion events, and that is exactly
// the case that has to keep scrolling.
namespace microide::workspace::selection_autoscroll {

// Rows/columns per tick, from how far the real (unclamped) pointer is past the
// band. Proportional so a small overshoot creeps and a large one moves quickly,
// capped so a pointer flung to the edge of a large display cannot page the
// document per tick. Zero (disarmed) whenever the pointer is inside the band.
void Arm(InteractionState& interaction_state,
         const SDL_FRect& editor_rect,
         const editor::EditorViewMetrics& metrics,
         float pointer_x,
         float pointer_y);

void Disarm(InteractionState& interaction_state);

// Idle-wake interval while armed, else nullopt. Slower than the 60 fps
// animations: this scrolls CONTENT, and a screenful per 16 ms is unreadable.
std::optional<Uint32> NextDelayMs(const InteractionState& interaction_state);

// Apply one step to `viewport`. Returns false -- and disarms -- when the drag is
// over or the viewport cannot scroll any further in the direction the pointer is
// pushing, which is what stops the wake from spinning at the end of a document.
//
// The caller re-extends the selection to the held pointer afterwards; that needs
// a coordinator and a layout, which is the one part that does belong to the
// shell.
bool Step(InteractionState& interaction_state, editor::TextViewport& viewport);

}  // namespace microide::workspace::selection_autoscroll
