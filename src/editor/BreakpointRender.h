#pragma once

#include <SDL3/SDL.h>

#include "render/Theme.h"

namespace microide::editor {

SDL_FRect BreakpointGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                     float line_height);

enum class BreakpointGutterKind { Plain, Conditional, Logpoint };

void DrawBreakpointGutterMarker(SDL_Renderer* renderer, const render::Theme& theme, float gutter_x,
                                float y, float gutter_width, float line_height, bool verified,
                                BreakpointGutterKind kind = BreakpointGutterKind::Plain,
                                bool enabled = true);

}
