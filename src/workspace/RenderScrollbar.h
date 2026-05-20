#pragma once

#include <SDL3/SDL.h>

namespace microide::render {
struct Theme;
}

namespace microide::workspace {

// Fill the scrollbar track rect with the theme's raised-surface color.
void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track);

// Fill the scrollbar thumb rect; `active` swaps the disabled-text fill for the
// accent color to indicate hover/drag.
void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false);

// Draw both the track and thumb in one call.
void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false);

}  // namespace microide::workspace
