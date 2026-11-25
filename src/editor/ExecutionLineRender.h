#pragma once

#include <SDL3/SDL.h>

#include "render/Theme.h"

namespace microide::editor {

SDL_FRect ExecutionLineGutterMarkerRect(float gutter_x, float y, float gutter_width,
                                        float line_height);

void DrawExecutionLineGutterMarker(SDL_Renderer* renderer, const render::Theme& theme,
                                   float gutter_x, float y, float gutter_width, float line_height);

}
