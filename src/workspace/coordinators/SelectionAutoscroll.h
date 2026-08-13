#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::editor {
class TextViewport;
struct EditorViewMetrics;
}  // namespace microide::editor

namespace microide::workspace {
struct TextGridInteractionLayout;
}  // namespace microide::workspace

// Selection-drag autoscroll.
//
// A drag whose pointer is held past an edge of the visible text band clamps onto
// the edge cell (see EditorMouseCoordinator::HandleSelectionMotion), so without
// this the selection stops growing at the first/last visible row and a drag can
// never select more than a screenful.
//
// It lives here rather than on WorkspaceShell because none of it needs the
// shell: arming reads a pointer against a pane's geometry, stepping produces two
// deltas, and disarming clears two ints. The shell keeps only the lines that
// connect it to the idle wake.
//
// The wake, not the motion handler, is what repeats the step: a pointer held
// still outside the band produces no further motion events, and that is exactly
// the case that has to keep scrolling.
namespace microide::workspace::selection_autoscroll {

// The visible text band of one pane, which is all the arming maths needs. Three
// surfaces describe their geometry with three different structs (the editor's
// `EditorViewMetrics`, compare's and merge's `TextGridInteractionLayout`), and
// the overshoot ramp does not care which -- so they converge here rather than
// each growing its own copy of the ramp.
struct Band {
  SDL_FRect rect{};
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  std::size_t visible_rows = 1;
};

Band BandFor(const SDL_FRect& pane_rect, const editor::EditorViewMetrics& metrics);
Band BandFor(const TextGridInteractionLayout& interaction);

// Rows/columns per tick, from how far the real (unclamped) pointer is past the
// band. Proportional so a small overshoot creeps and a large one moves quickly,
// capped so a pointer flung to the edge of a large display cannot page the
// document per tick. Zero (disarmed) whenever the pointer is inside the band.
void Arm(InteractionState& interaction_state,
         const Band& band,
         float pointer_x,
         float pointer_y);

void Disarm(InteractionState& interaction_state);

// Idle-wake interval while armed, else nullopt. Slower than the 60 fps
// animations: this scrolls CONTENT, and a screenful per 16 ms is unreadable.
std::optional<Uint32> NextDelayMs(const InteractionState& interaction_state);

struct StepDelta {
  int rows = 0;
  int columns = 0;
};

// The scroll this tick asks for, or nullopt when the drag is over (and then it
// has already disarmed). The caller applies the deltas to whatever holds its
// scroll -- a `TextViewport`, a compare tab's `scroll_row` and its sync helper,
// the merge result pane -- and reports back through `FinishStep`.
//
// Split in two rather than taking a viewport because compare and merge do not
// scroll a viewport: they scroll their own tab state through their own clamping
// helpers, and a step that reached past them would fight those helpers.
std::optional<StepDelta> BeginStep(InteractionState& interaction_state);

// `moved` is whether the surface's scroll actually changed. False means the
// pointer is pushing at an end of the document, so this disarms and returns
// false -- which is what stops the wake from spinning there.
bool FinishStep(InteractionState& interaction_state, bool moved);

// BeginStep/apply/FinishStep for a surface whose scroll IS a viewport (the
// editor pane, and the merge result pane once its mirror is re-synced).
bool Step(InteractionState& interaction_state, editor::TextViewport& viewport);

}  // namespace microide::workspace::selection_autoscroll
