#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "terminal/TerminalSession.h"

namespace microide::workspace {

struct TerminalSelectionPoint {
  std::size_t row = 0;
  std::size_t column = 0;
};

struct TerminalSelectionBounds {
  TerminalSelectionPoint start{};
  TerminalSelectionPoint end{};
};

std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head);
terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button);
std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection);
bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column);

}  // namespace microide::workspace
