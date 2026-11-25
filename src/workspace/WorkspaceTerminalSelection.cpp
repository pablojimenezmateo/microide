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

namespace {

std::size_t TerminalLineContentEnd(const terminal::TerminalLine& line) {
  std::size_t end = line.cells.size();
  while (end > 0) {
    const std::string_view text = line.cells[end - 1].DisplayText();
    if (!text.empty() && text != " ") {
      break;
    }
    --end;
  }
  return end;
}

}

std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection,
                                         std::size_t max_bytes) {
  if (lines.empty() || selection.start.row >= lines.size()) {
    return {};
  }
  const std::size_t end_row = std::min(selection.end.row, lines.size() - 1);
  TerminalSelectionTextBuilder builder(selection, max_bytes);
  for (std::size_t row = selection.start.row; row <= end_row; ++row) {
    if (!builder.AddRow(row, lines[row])) {
      break;
    }
  }
  return builder.Finish(end_row + 1 < lines.size() && lines[end_row + 1].wrapped_from_previous);
}

bool TerminalSelectionTextBuilder::AddRow(std::size_t row, const terminal::TerminalLine& line) {
  if (done_) {
    return false;
  }
  if (pending_ && !SettlePending(line.wrapped_from_previous, true)) {
    return false;
  }
  const std::size_t line_size = line.cells.size();
  const std::size_t start_column = row == selection_.start.row ? std::min(selection_.start.column, line_size) : 0;
  const std::size_t end_column = row == selection_.end.row ? std::min(selection_.end.column, line_size) : line_size;
  const std::size_t content_end =
      std::max(start_column, std::min(end_column, TerminalLineContentEnd(line)));
  AppendTerminalLineSlice(text_, line, start_column, content_end);
  pending_content_end_ = text_.size();
  AppendTerminalLineSlice(text_, line, content_end, end_column);
  pending_ = true;
  return true;
}

bool TerminalSelectionTextBuilder::SettlePending(bool continued, bool more_rows) {
  pending_ = false;
  if (!continued) {
    text_.resize(pending_content_end_);
  }
  if (text_.size() >= max_bytes_) {
    text_.resize(util::Utf8ByteBudgetPrefixLength(text_, max_bytes_));
    text_ += "\n[selection truncated]";
    done_ = true;
    return false;
  }
  if (more_rows && !continued) {
    text_.push_back('\n');
  }
  return true;
}

std::string TerminalSelectionTextBuilder::Finish(bool next_row_continues) {
  if (pending_) {
    (void)SettlePending(next_row_continues, false);
  }
  done_ = true;
  return std::move(text_);
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
    if (transcript.size() >= max_bytes) {
      transcript.resize(util::Utf8ByteBudgetPrefixLength(transcript, max_bytes));
      transcript += "\n[output truncated]";
      return transcript;
    }
  }
  if (source_truncated) {
    transcript += "\n[output truncated]";
  }
  return transcript;
}

namespace {

bool IsTerminalWordCell(const terminal::TerminalCell& cell) {
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

}

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
  const std::size_t end = TerminalLineContentEnd(line);
  if (end == 0) {
    return std::nullopt;
  }
  return TerminalSelectionBounds{.start = {row, 0}, .end = {row, end}};
}

TerminalSelectionEnds ExtendTerminalSelection(TerminalSelectionUnit unit,
                                              const TerminalSelectionBounds& origin,
                                              const terminal::TerminalLine& line,
                                              TerminalSelectionPoint point) {
  const auto before = [](TerminalSelectionPoint a, TerminalSelectionPoint b) {
    return a.row < b.row || (a.row == b.row && a.column < b.column);
  };
  TerminalSelectionBounds target{.start = point, .end = point};
  switch (unit) {
    case TerminalSelectionUnit::Cell:
      return TerminalSelectionEnds{.anchor = origin.start, .head = point};
    case TerminalSelectionUnit::Word:
      if (auto word = TerminalWordBoundsAt(line, point.row, point.column); word.has_value()) {
        target = *word;
      } else if (point.column > 0) {
        if (auto left = TerminalWordBoundsAt(line, point.row, point.column - 1); left.has_value()) {
          target = *left;
        }
      }
      break;
    case TerminalSelectionUnit::Line:
      target = TerminalSelectionBounds{.start = {point.row, 0}, .end = {point.row, TerminalLineContentEnd(line)}};
      break;
  }
  if (before(target.start, origin.start)) {
    return TerminalSelectionEnds{.anchor = origin.end, .head = target.start};
  }
  return TerminalSelectionEnds{.anchor = origin.start, .head = before(origin.end, target.end) ? target.end : origin.end};
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

std::string TrimTrailingTerminalBlanks(std::string text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

std::string TerminalLineText(const terminal::TerminalLine& line) {
  return TerminalLineSliceText(line, 0, line.cells.size(), true);
}

std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing) {
  std::string text;
  text.reserve(std::min(end, line.cells.size()) - std::min(start, std::min(end, line.cells.size())));
  AppendTerminalLineSlice(text, line, start, end);
  return trim_trailing ? TrimTrailingTerminalBlanks(std::move(text)) : text;
}

void AppendTerminalLineSlice(std::string& out,
                             const terminal::TerminalLine& line,
                             std::size_t start,
                             std::size_t end) {
  const std::size_t clamped_start = std::min(start, line.cells.size());
  const std::size_t clamped_end = std::min(std::max(clamped_start, end), line.cells.size());
  for (std::size_t column = clamped_start; column < clamped_end; ++column) {
    const auto& cell = line.cells[column];
    if (cell.style.wide_trailing()) {
      continue;
    }
    const auto display_text = cell.DisplayText();
    if (!display_text.empty()) {
      out.append(display_text);
      continue;
    }
    out.push_back(' ');
  }
}

}
