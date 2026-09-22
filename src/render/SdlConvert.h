#pragma once

#include <SDL3/SDL.h>

#include "util/KeyModifiers.h"
#include "util/Rgba8.h"

// The one boundary between the kernel's plain input/colour vocabulary and SDL's.
// Kernel code (the terminal model, the shared ANSI palette) produces util::Rgba8 and
// consumes util::KeyModifiers; anything that paints or reads SDL events converts here.
namespace microide::render {

constexpr SDL_Color ToSdlColor(util::Rgba8 color) {
  return SDL_Color{color.r, color.g, color.b, color.a};
}

constexpr util::Rgba8 ToRgba8(SDL_Color color) {
  return util::Rgba8{color.r, color.g, color.b, color.a};
}

constexpr util::KeyModifiers ToKeyModifiers(SDL_Keymod modifiers) {
  util::KeyModifiers result = util::kKeyModNone;
  if ((modifiers & SDL_KMOD_SHIFT) != 0) {
    result |= util::kKeyModShift;
  }
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    result |= util::kKeyModCtrl;
  }
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    result |= util::kKeyModAlt;
  }
  if ((modifiers & SDL_KMOD_GUI) != 0) {
    result |= util::kKeyModGui;
  }
  return result;
}

}  // namespace microide::render
