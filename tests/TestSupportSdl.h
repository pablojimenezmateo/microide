#pragma once

#include <SDL3/SDL.h>

namespace microide::tests {

void EnsureDummySdlVideoInitialized();
void ResetSdlModStateForTests();

class ScopedSdlModState {
 public:
  explicit ScopedSdlModState(SDL_Keymod modifiers);
  ~ScopedSdlModState();

  ScopedSdlModState(const ScopedSdlModState&) = delete;
  ScopedSdlModState& operator=(const ScopedSdlModState&) = delete;

 private:
  SDL_Keymod previous_mods_ = SDL_KMOD_NONE;
};

}
