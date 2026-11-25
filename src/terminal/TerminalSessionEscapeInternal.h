#pragma once

#include "terminal/TerminalCell.h"

#include <cstddef>
#include <string_view>

namespace microide::terminal::detail {

[[nodiscard]] bool ApplySgrParameters(TerminalStyle& style, TerminalColor& underline_color,
                                      std::string_view body);

void InsertBlankCells(TerminalLine& line, std::size_t at, std::size_t count, std::size_t columns,
                      const TerminalStyle& style);

}
