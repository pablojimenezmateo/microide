#pragma once

#include <SDL3/SDL.h>

#include "render/Theme.h"

namespace microide::editor {

// Geometry of the debugger execution arrow for one gutter row: a small square
// near the left edge of the gutter, sized + positioned like the breakpoint dot
// so the two line up when a session stops on a breakpoint. Returns {x, y, d, d}.
SDL_FRect ExecutionLineGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                        float line_height);

// Paint a right-pointing filled arrow (the "current execution line" marker) in
// `theme.debug_execution_arrow`, drawn on top of any breakpoint dot on the row.
void DrawExecutionLineGutterMarker(SDL_Renderer* renderer, const render::Theme& theme,
                                   float gutter_x, float y, float gutter_width, float line_height);

}  // namespace microide::editor
