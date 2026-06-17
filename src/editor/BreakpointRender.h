#pragma once

#include <SDL3/SDL.h>

#include "render/Theme.h"

namespace microide::editor {

// Geometry of the breakpoint dot for one gutter row: a small circle near the
// left edge of the gutter, clear of the diagnostic bar (gutter_x + 2) and the
// fold marker (rightmost ~14px). Returns the bounding square {x, y, d, d}.
SDL_FRect BreakpointGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                     float line_height);

// Visual flavor of a breakpoint gutter marker (Phase 9). Plain breakpoints are a
// filled disc; conditional / hit-count breakpoints reuse the disc in a distinct
// tint; logpoints render as a filled diamond (a shape cue that they never pause).
enum class BreakpointGutterKind { Plain, Conditional, Logpoint };

// Paint the breakpoint marker. `verified` picks the solid theme color; otherwise
// the dimmed unverified color is used (breakpoint set but not adapter-bound).
// `kind` selects the shape/tint (see BreakpointGutterKind).
void DrawBreakpointGutterMarker(SDL_Renderer* renderer, const render::Theme& theme, float gutter_x,
                                float y, float gutter_width, float line_height, bool verified,
                                BreakpointGutterKind kind = BreakpointGutterKind::Plain);

}  // namespace microide::editor
