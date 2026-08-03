#pragma once

#include <array>

#include <SDL3/SDL.h>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

// Grab rects for the compare and merge pane dividers. Pure functions of the
// surface rect plus the resolved surface layout — no shell state — so they live
// outside WorkspaceShell rather than adding to its member surface. Three callers
// share each: the mouse coordinator that starts the drag, the cursor resolver
// that shows the resize shape, and the test access that drives both.
//
// The painted divider is about one glyph wide, which is fiddly to hit, so the
// grab is widened to kDiffDividerHitWidth without widening what is drawn. Merge
// used to skip that and hit-test the painted width directly.
inline constexpr float kDiffDividerHitWidth = 12.0f;

SDL_FRect CompareDividerHitRect(const SDL_FRect& editor_surface,
                                const WorkspaceShell::CompareSurfaceLayout& surface);

// {left, right} in pane order.
std::array<SDL_FRect, 2> MergeDividerHitRects(const SDL_FRect& editor_surface,
                                              const WorkspaceShell::MergeSurfaceLayout& surface);

}  // namespace microide::workspace
