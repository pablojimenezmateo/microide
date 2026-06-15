#pragma once

#include <SDL3/SDL.h>

// Shared ANSI / xterm-256 colour palette. The 16-colour table and the
// 6x6x6 cube + grayscale ramp were previously duplicated byte-for-byte between
// the theme parser and the terminal emulator; both now resolve colours here so
// the palette has a single definition.
namespace microide::render {

// Resolve one of the 16 base ANSI colours (index 0-7, optionally bright).
// Out-of-range indices are clamped to [0, 7].
SDL_Color BasicAnsiColor(int index, bool bright);

// Resolve an xterm-256 colour index: 0-15 base colours, 16-231 the 6x6x6 RGB
// cube, 232-255 the grayscale ramp. Negative indices fall back to colour 0.
SDL_Color Ansi256Color(int index);

}  // namespace microide::render
