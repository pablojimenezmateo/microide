#include "workspace/WorkspaceTerminalSelection.h"

#include <algorithm>

namespace microide::workspace {

std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head) {
  if (!anchor.has_value() || !head.has_value()) {
    return std::nullopt;
  }

  TerminalSelectionBounds bounds{.start = *anchor, .end = *head};
  if (bounds.start.row > bounds.end.row ||
      (bounds.start.row == bounds.end.row && bounds.start.column > bounds.end.column)) {
    std::swap(bounds.start, bounds.end);
  }
  return bounds;
}

terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return terminal::TerminalSession::MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
      return terminal::TerminalSession::MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
      return terminal::TerminalSession::MouseButton::Right;
    default:
      return terminal::TerminalSession::MouseButton::None;
  }
}

std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection) {
  if (lines.empty() || selection.start.row >= lines.size()) {
    return {};
  }

  const std::size_t end_row = std::min(selection.end.row, lines.size() - 1);
  std::string text;
  for (std::size_t row = selection.start.row; row <= end_row; ++row) {
    const auto& line = lines[row];
    const std::size_t line_size = line.cells.size();
    const std::size_t start_column =
        row == selection.start.row ? std::min(selection.start.column, line_size) : 0;
    const std::size_t end_column =
        row == end_row ? std::min(selection.end.column, line_size) : line_size;
    for (std::size_t column = start_column; column < end_column; ++column) {
      text.append(line.cells[column].DisplayText());
    }
    if (row != end_row) {
      text.push_back('\n');
    }
  }
  return text;
}

bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column) {
  if (row < selection.start.row || row > selection.end.row) {
    return false;
  }
  if (selection.start.row == selection.end.row) {
    return row == selection.start.row && column >= selection.start.column &&
           column < selection.end.column;
  }
  if (row == selection.start.row) {
    return column >= selection.start.column;
  }
  if (row == selection.end.row) {
    return column < selection.end.column;
  }
  return true;
}

}  // namespace microide::workspace
