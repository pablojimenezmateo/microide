#pragma once

#include "util/Rgba8.h"

// Shared ANSI / xterm-256 colour palette. The 16-colour table and the
// 6x6x6 cube + grayscale ramp were previously duplicated byte-for-byte between
// the theme parser and the terminal emulator; both now resolve colours here so
// the palette has a single definition. It lives in util, not render, because the
// terminal model is kernel code and must not include the windowing library —
// `terminal::BasicAnsiColor` used to be a forwarder to the render copy, which is
// also why the "parity" test between the two could never fail.
namespace microide::util {

// Resolve one of the 16 base ANSI colours (index 0-7, optionally bright).
// Out-of-range indices are clamped to [0, 7].
Rgba8 BasicAnsiColor(int index, bool bright);

// Resolve an xterm-256 colour index: 0-15 base colours, 16-231 the 6x6x6 RGB
// cube, 232-255 the grayscale ramp. Negative indices fall back to colour 0.
Rgba8 Ansi256Color(int index);

}  // namespace microide::util
