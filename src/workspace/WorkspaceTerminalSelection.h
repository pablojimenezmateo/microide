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
// Slices a single terminal row into copyable text between [start, end) columns.
// Skips the trailing spacer cell of a double-width glyph and renders empty
// cells as a single space so that internal spacing is preserved; trailing
// blanks are trimmed only when trim_trailing is set. Shared by whole-line copy
// and selection copy so both paths produce identical text.
std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing);
// Default byte budget for a single terminal selection copy. Generous enough for any
// real selection, but bounded so a drag over the full 100k-line scrollback cap cannot
// materialize an unbounded transcript on the UI thread (TD-2026-07-17A-090).
inline constexpr std::size_t kDefaultTerminalSelectionCopyMaxBytes = 8u * 1024u * 1024u;
// Extract the selected text. If the accumulated bytes reach `max_bytes`, the result is
// truncated on a UTF-8 boundary and a "\n[selection truncated]" marker is appended.
std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection,
                                         std::size_t max_bytes = kDefaultTerminalSelectionCopyMaxBytes);
bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column);

}  // namespace microide::workspace
