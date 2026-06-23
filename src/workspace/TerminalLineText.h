#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "terminal/TerminalCell.h"

namespace microide::workspace {

// Drop trailing NULs/spaces a terminal grid pads cells with, so copied or
// snapshotted text does not carry phantom blanks.
inline std::string TrimTrailingTerminalBlanks(std::string text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

// Render `[start, end)` of a terminal line to UTF-8, skipping the trailing
// spacer cell of double-width glyphs (it owns no text of its own).
inline std::string TerminalLineSliceText(const terminal::TerminalLine& line, std::size_t start,
                                         std::size_t end, bool trim_trailing) {
  const std::size_t clamped_start = std::min(start, line.cells.size());
  const std::size_t clamped_end = std::min(std::max(clamped_start, end), line.cells.size());
  std::string text;
  text.reserve(clamped_end - clamped_start);
  for (std::size_t column = clamped_start; column < clamped_end; ++column) {
    const auto& cell = line.cells[column];
    if (cell.style.wide_trailing()) {
      continue;
    }
    const auto display_text = cell.DisplayText();
    if (!display_text.empty()) {
      text.append(display_text);
      continue;
    }
    text.push_back(' ');
  }
  return trim_trailing ? TrimTrailingTerminalBlanks(std::move(text)) : text;
}

// Render a whole terminal line to trimmed UTF-8.
inline std::string TerminalLineText(const terminal::TerminalLine& line) {
  return TerminalLineSliceText(line, 0, line.cells.size(), true);
}

}  // namespace microide::workspace
