#pragma once

#include <SDL3/SDL.h>

namespace microide::app {

struct RenderPresentation {
  int logical_width = 0;
  int logical_height = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;

  bool operator==(const RenderPresentation&) const = default;
};

inline bool ApplyRenderPresentation(SDL_Renderer* renderer, const RenderPresentation& presentation) {
  if (!SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
    return false;
  }
  return SDL_SetRenderScale(renderer, presentation.scale_x, presentation.scale_y);
}

}
