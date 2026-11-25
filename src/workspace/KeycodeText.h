#pragma once

#include <SDL3/SDL.h>

namespace microide::workspace {

char KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers);

}
