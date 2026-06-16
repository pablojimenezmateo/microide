#pragma once

#include "terminal/TerminalCell.h"

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

}  // namespace microide::terminal::detail
