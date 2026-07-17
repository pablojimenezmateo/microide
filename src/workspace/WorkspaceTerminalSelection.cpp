#include "workspace/WorkspaceTerminalSelection.h"

#include <algorithm>

#include "util/StringUtil.h"

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
                                         const TerminalSelectionBounds& selection,
                                         std::size_t max_bytes) {
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
    // Reuse the whole-line slice logic so selection copy and line copy produce
    // identical text: wide-trailing spacer cells are skipped and empty cells
    // render as a single space. Trailing blanks are preserved per row because a
    // mid-selection span may legitimately end on blank cells.
    text.append(TerminalLineSliceText(line, start_column, end_column, /*trim_trailing=*/false));
    // Cap the copied bytes so a drag over the full scrollback can't materialize an
    // unbounded transcript on the UI thread (TD-2026-07-17A-090). Truncate on a UTF-8
    // boundary and report truncation rather than silently clipping mid-codepoint.
    if (text.size() >= max_bytes) {
      std::size_t cut = std::min(text.size(), max_bytes);
      while (cut > 0 && util::IsUtf8ContinuationByte(static_cast<unsigned char>(text[cut]))) {
        --cut;
      }
      text.resize(cut);
      text += "\n[selection truncated]";
      return text;
    }
    if (row != end_row) {
      // Only emit a hard newline at a *real* line boundary. If the next row is a
      // soft-wrap continuation of this one, the terminal never saw a newline
      // there, so copying a long wrapped command must not inject one either.
      if (!lines[row + 1].wrapped_from_previous) {
        text.push_back('\n');
      }
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
