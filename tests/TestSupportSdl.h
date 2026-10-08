#pragma once

// The SDL half of the test support. Kept out of TestSupport.h so the kernel test
// binary (microide_kernel_tests), which links no windowing library, can share the
// rest of it: a kernel test that reaches for SDL now fails to compile instead of
// quietly linking SDL into a binary whose point is that it has none.

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

}  // namespace microide::tests
