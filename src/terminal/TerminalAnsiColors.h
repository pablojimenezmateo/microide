#pragma once

#include "terminal/TerminalSession.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace microide::terminal {

SDL_Color MakeTerminalRgbColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff);
SDL_Color BasicAnsiColor(int index, bool bright);
SDL_Color Ansi256Color(int index);
TerminalCell MakeAsciiTerminalCell(char character, const TerminalStyle& style);
TerminalCell MakeUtf8TerminalCell(std::string_view glyph, const TerminalStyle& style);

}  // namespace microide::terminal
