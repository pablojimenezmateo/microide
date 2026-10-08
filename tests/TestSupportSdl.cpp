#include "TestSupportSdl.h"

#include "TestSupport.h"

#include <stdexcept>
#include <string>

namespace microide::tests {

void EnsureDummySdlVideoInitialized() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  if (!initialized) {
    throw std::runtime_error(
        std::string("SDL initialization failed for tests: ") + SDL_GetError());
  }
}

void ResetSdlModStateForTests() {
  SDL_SetModState(SDL_KMOD_NONE);
}

ScopedSdlModState::ScopedSdlModState(SDL_Keymod modifiers) : previous_mods_(SDL_GetModState()) {
  SDL_SetModState(modifiers);
}

ScopedSdlModState::~ScopedSdlModState() {
  SDL_SetModState(previous_mods_);
}

}  // namespace microide::tests
