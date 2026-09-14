#pragma once

namespace microide::workspace {

// The merge toolbar's button metrics, in ONE place.
//
// These were two copies, one in the layout TU that computes the button rects the
// mouse coordinator hit-tests, and one in the render TU that re-derived the same
// three secondary-row rects with the same arithmetic. They agreed, so nothing
// showed -- but a change to either copy moves the drawn button away from the
// clickable one, which is the failure `dev-docs/project/ui-invariants.md` § Drags
// already records for the drop target. The render TU now asks
// `ComputeMergeSecondaryToolbarButtonRect` for the rects instead of rebuilding
// them, and what remains shared is just the two numbers.
inline constexpr float kMergeToolbarButtonHeight = 22.0f;
inline constexpr float kMergeToolbarButtonGap = 8.0f;

}  // namespace microide::workspace
