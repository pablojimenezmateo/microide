#pragma once

#include "terminal/TerminalSession.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace microide::terminal {

TerminalCell MakeAsciiTerminalCell(char character, const TerminalStyle& style);
TerminalCell MakeUtf8TerminalCell(std::string_view glyph, const TerminalStyle& style);

}  // namespace microide::terminal
