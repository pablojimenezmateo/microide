#pragma once

#include <SDL3/SDL.h>

namespace microide::app {

// How a render target maps layout units to its device pixels: the layout's size
// in units and the exact scale. Every target the shell draws into -- the window,
// the retained scene texture, the redraw verifier's reference -- gets the same
// one, so the shell's drawing lands on the same device pixels in each.
//
// The scale is the requested one (display density times ui.scale), applied as
// the target's render scale, not derived from a logical size. SDL's STRETCH
// logical presentation made it pixels / units, and with the units a whole number
// that is not the requested scale unless the window happens to be a multiple of
// it: a 1024-pixel window at 1.25 was 819 units at 1.2503. At such a scale no
// whole number of units is a whole number of device pixels, so a surface's clip
// and a partial frame's damage clip could not be aligned (ScopedRenderClip), and
// SDL's ceil of a clip's length put one device column of the sidebar's text into
// the editor in partial frames only (QA monkey, qa lane seed 1).
struct RenderPresentation {
  int logical_width = 0;
  int logical_height = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;

  bool operator==(const RenderPresentation&) const = default;
};

// Applies `presentation` to the renderer's current target. Logical presentation
// is off: the units overhang the target by under one unit at the right and
// bottom (util::ComputeWindowPresentation rounds them up), which the target's
// edge clips, so every device pixel is drawn.
inline bool ApplyRenderPresentation(SDL_Renderer* renderer, const RenderPresentation& presentation) {
  if (!SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
    return false;
  }
  return SDL_SetRenderScale(renderer, presentation.scale_x, presentation.scale_y);
}

}  // namespace microide::app
