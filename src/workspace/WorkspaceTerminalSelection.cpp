#include "workspace/WorkspaceTerminalSelection.h"

#include <algorithm>

#include "editor/EditTypes.h"
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
      text.resize(util::Utf8ByteBudgetPrefixLength(text, max_bytes));
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

std::string BuildLastTerminalCommandTranscript(
    const std::vector<std::string>& rows,
    std::string_view trimmed_prompt_prefix,
    std::string_view invocation_first_line,
    bool source_truncated,
    std::size_t max_bytes) {
  std::size_t end_row = rows.size();
  while (end_row > 0 && rows[end_row - 1].empty()) {
    --end_row;
  }
  if (end_row == 0) {
    return {};
  }

  // Drop the trailing prompt row the shell redraws after the command finishes: either a
  // literal re-print of the captured prompt prefix, or (in echo-off/partial-redraw
  // shells) a leading fragment of the invocation itself.
  const std::string& last_line = rows[end_row - 1];
  if ((!trimmed_prompt_prefix.empty() &&
       (last_line == trimmed_prompt_prefix || last_line.starts_with(trimmed_prompt_prefix))) ||
      (!last_line.empty() && invocation_first_line.size() > last_line.size() &&
       invocation_first_line.starts_with(last_line))) {
    --end_row;
    while (end_row > 0 && rows[end_row - 1].empty()) {
      --end_row;
    }
  }
  if (end_row == 0) {
    return {};
  }

  std::string transcript;
  for (std::size_t row = 0; row < end_row; ++row) {
    if (!transcript.empty()) {
      transcript.push_back('\n');
    }
    transcript += rows[row];
    // Cap the joined bytes so a command with a huge captured output can't materialize an
    // unbounded transcript on the UI thread (TD-2026-07-17A-037). Truncate on a UTF-8
    // boundary and report truncation rather than clipping mid-codepoint.
    if (transcript.size() >= max_bytes) {
      transcript.resize(util::Utf8ByteBudgetPrefixLength(transcript, max_bytes));
      transcript += "\n[output truncated]";
      return transcript;
    }
  }
  // Rows were dropped upstream to honor the snapshot line cap: mark the tail as truncated
  // even though the retained bytes fit the budget.
  if (source_truncated) {
    transcript += "\n[output truncated]";
  }
  return transcript;
}

namespace {

// A cell counts as part of a double-click word when it holds a single-byte
// identifier char or one of the path/URL joiners a terminal token is made of.
// Multi-byte cells are treated as word bytes so a UTF-8 identifier is not split
// mid-token by the click.
bool IsTerminalWordCell(const terminal::TerminalCell& cell) {
  // The trailing spacer of a double-width glyph carries no text of its own but is
  // part of the glyph in front of it, so it must not break a word — otherwise a
  // double-click on CJK text stops at every second column. TerminalLineSliceText
  // skips these cells when copying for the same reason.
  if (cell.style.wide_trailing()) {
    return true;
  }
  const std::string_view text = cell.DisplayText();
  if (text.empty() || text == " ") {
    return false;
  }
  if (text.size() > 1) {
    return true;
  }
  const char ch = text.front();
  return editor::IsIdentifierByte(ch) || ch == '.' || ch == '-' || ch == '/' || ch == '~' ||
         ch == '+' || ch == ':' || ch == '@';
}

}  // namespace

std::optional<TerminalSelectionBounds> TerminalWordBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row,
                                                            std::size_t column) {
  if (column >= line.cells.size() || !IsTerminalWordCell(line.cells[column])) {
    return std::nullopt;
  }
  std::size_t start = column;
  while (start > 0 && IsTerminalWordCell(line.cells[start - 1])) {
    --start;
  }
  std::size_t end = column + 1;
  while (end < line.cells.size() && IsTerminalWordCell(line.cells[end])) {
    ++end;
  }
  return TerminalSelectionBounds{.start = {row, start}, .end = {row, end}};
}

std::optional<TerminalSelectionBounds> TerminalLineBoundsAt(const terminal::TerminalLine& line,
                                                            std::size_t row) {
  std::size_t end = line.cells.size();
  while (end > 0) {
    const std::string_view text = line.cells[end - 1].DisplayText();
    if (!text.empty() && text != " ") {
      break;
    }
    --end;
  }
  if (end == 0) {
    return std::nullopt;
  }
  return TerminalSelectionBounds{.start = {row, 0}, .end = {row, end}};
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
