#pragma once

#include <SDL3/SDL.h>

namespace microide::workspace {

// The ASCII character a US-layout key types under `modifiers` (Shift only), or
// '\0' for a key that types none. For the paths that see a key before -- or
// instead of -- its SDL text event.
char KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers);

}  // namespace microide::workspace
