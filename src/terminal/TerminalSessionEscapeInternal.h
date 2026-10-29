#pragma once

#include "terminal/TerminalCell.h"

#include <cstddef>
#include <string_view>

// Internal seam shared between the escape-handling translation units that were
// split out of the former TerminalSessionEscape.cpp (SGR / CSI / OSC / private
// modes). Only the SGR application entry point needs to cross a translation-unit
// boundary: the CSI dispatcher (TerminalSessionCsi.cpp) invokes it, while the
// implementation and its colour-parsing helpers live in TerminalSessionSgr.cpp.
namespace microide::terminal::detail {

// Apply an SGR (Select Graphic Rendition) parameter body (`CSI ... m`) to the
// given style, mutating bold/italic/colour/underline/etc. attributes in place.
void ApplySgrParameters(TerminalStyle& style, std::string_view body);

// Open `count` blank cells (in `style`) at `at`, shifting the rest of the row
// right; cells pushed past the right margin `columns` are lost, and a wide pair
// the insert splits or the margin cuts is blanked. ICH (TerminalSessionCsi.cpp)
// and IRM's per-glyph insert (TerminalSessionScreen.cpp) share it.
void InsertBlankCells(TerminalLine& line, std::size_t at, std::size_t count, std::size_t columns,
                      const TerminalStyle& style);

}  // namespace microide::terminal::detail
