#include "workspace/WorkspaceShell.h"

#include <cctype>
#include <cmath>
#include <string_view>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

namespace {

constexpr float kBottomPanelTextInset = 12.0f;
constexpr float kBottomPanelTextTopInset = 8.0f;
constexpr float kBottomPanelScrollbarTextReserve = 16.0f;

void EraseLastUtf8Codepoint(std::string& text) {
  if (text.empty()) {
    return;
  }

  std::size_t erase_pos = text.size() - 1;
  while (erase_pos > 0 &&
         (static_cast<unsigned char>(text[erase_pos]) & 0b1100'0000U) == 0b1000'0000U) {
    --erase_pos;
  }
  text.erase(erase_pos);
}

std::string TrimTrailingTerminalBlanks(std::string text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing) {
  const std::size_t clamped_start = std::min(start, line.cells.size());
  const std::size_t clamped_end = std::min(std::max(clamped_start, end), line.cells.size());
  std::string text;
  text.reserve(clamped_end - clamped_start);
  for (std::size_t column = clamped_start; column < clamped_end; ++column) {
    const auto display_text = line.cells[column].DisplayText();
    if (!display_text.empty()) {
      text.append(display_text);
      continue;
    }
    text.push_back(' ');
  }
  return trim_trailing ? TrimTrailingTerminalBlanks(std::move(text)) : text;
}

std::string TerminalLineText(const terminal::TerminalLine& line) {
  return TerminalLineSliceText(line, 0, line.cells.size(), true);
}

std::string FirstLine(std::string_view text) {
  const std::size_t newline = text.find('\n');
  return std::string(text.substr(0, newline));
}

bool IsTerminalUrlTerminator(char character) {
  return std::isspace(static_cast<unsigned char>(character)) != 0 || character == '"' ||
         character == '\'' || character == '<' || character == '>';
}

std::string TrimTerminalUrl(std::string url) {
  while (!url.empty()) {
    const char tail = url.back();
    if (tail == '.' || tail == ',' || tail == ';' || tail == ':' || tail == '!' ||
        tail == '?' || tail == ')' || tail == ']' || tail == '}') {
      url.pop_back();
      continue;
    }
    break;
  }
  return url;
}

std::optional<std::string> TerminalUrlAtColumn(std::string_view text, std::size_t column) {
  static constexpr std::string_view kSchemes[] = {
      "https://",
      "http://",
      "ftp://",
      "file://",
      "git://",
  };

  for (std::string_view scheme : kSchemes) {
    std::size_t start = text.find(scheme);
    while (start != std::string_view::npos) {
      std::size_t end = start + scheme.size();
      while (end < text.size() && !IsTerminalUrlTerminator(text[end])) {
        ++end;
      }
      std::string url = TrimTerminalUrl(std::string(text.substr(start, end - start)));
      const std::size_t trimmed_end = start + url.size();
      if (column >= start && column < trimmed_end && !url.empty()) {
        return url;
      }
      start = text.find(scheme, start + 1);
    }
  }

  return std::nullopt;
}

struct CapturedTerminalInvocation {
  std::size_t start_row = 0;
  std::string text;
};

CapturedTerminalInvocation CaptureVisibleTerminalInvocation(
    const std::vector<terminal::TerminalLine>& lines,
    std::size_t cursor_row,
    std::size_t cursor_column) {
  if (lines.empty()) {
    return {};
  }

  const std::size_t clamped_row = std::min(cursor_row, lines.size() - 1);
  std::size_t start_row = clamped_row;
  while (start_row > 0 && lines[start_row].wrapped_from_previous) {
    --start_row;
  }

  std::string text;
  for (std::size_t row = start_row; row <= clamped_row; ++row) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    const std::size_t end =
        row == clamped_row ? std::min(cursor_column, lines[row].cells.size()) : lines[row].cells.size();
    text += TerminalLineSliceText(lines[row], 0, end, false);
  }

  return CapturedTerminalInvocation{
      .start_row = start_row,
      .text = TrimTrailingTerminalBlanks(std::move(text)),
  };
}

std::size_t FindWrappedInvocationStartRow(const terminal::TerminalSession& session,
                                          std::size_t cursor_row) {
  std::size_t start_row = cursor_row;
  while (start_row > 0) {
    const auto current_line = session.SnapshotLineRange(start_row, 1);
    if (current_line.empty() || !current_line.front().wrapped_from_previous) {
      break;
    }
    --start_row;
  }
  return start_row;
}

}  // namespace
bool WorkspaceShell::BottomPanelVisible() const {
  return panel_state_.command_mode || !terminal_tabs_.empty();
}

WorkspaceShell::BottomPanelLogLayout WorkspaceShell::ComputeBottomPanelLogLayout(
    const WorkspaceLayout& layout,
    std::size_t line_count) const {
  BottomPanelLogLayout panel_layout;
  panel_layout.content_rect = BottomPanelContentRect(layout, panel_state_.command_mode);
  panel_layout.text_x = panel_layout.content_rect.x + kBottomPanelTextInset;
  panel_layout.text_y = panel_layout.content_rect.y + kBottomPanelTextTopInset;
  panel_layout.line_height = text_renderer_.LineHeight();

  const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
  const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
  panel_layout.scroll =
      ComputeScrollSurfaceLayout(panel_layout.content_rect, line_count, visible_rows, scroll_row);
  panel_layout.text_width =
      std::max(0.0f, panel_layout.content_rect.w - kBottomPanelTextInset * 2.0f -
                         (panel_layout.scroll.show_vertical ? kBottomPanelScrollbarTextReserve
                                                            : 0.0f));
  return panel_layout;
}

int WorkspaceShell::BottomPanelVisibleRows(float panel_height) const {
  return BottomPanelVisibleRowsForHeight(panel_height, text_renderer_.LineHeight(), panel_state_.command_mode);
}

int WorkspaceShell::BottomPanelScrollRow(std::size_t line_count, int visible_rows) const {
  const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
  if (const auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    return terminal_tab->follow_tail ? max_scroll
                                     : ClampScrollRowToContent(terminal_tab->scroll_row,
                                                               line_count,
                                                               visible_rows);
  }
  return 0;
}

std::optional<std::string> WorkspaceShell::TerminalUrlAtPoint(float x, float y) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const BottomPanelLogLayout panel_layout = ComputeBottomPanelLogLayout(*layout_state, line_count);
  const std::size_t first_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const auto lines = terminal_tab->session.SnapshotLineRange(
      first_row, static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows)));
  const auto position =
      TerminalSelectionPositionForPoint(static_cast<int>(std::lround(x)),
                                        static_cast<int>(std::lround(y)), lines, first_row);
  if (!position.has_value() || position->row < first_row ||
      position->row - first_row >= lines.size()) {
    return std::nullopt;
  }

  return TerminalUrlAtColumn(TerminalLineText(lines[position->row - first_row]),
                             position->column);
}

bool WorkspaceShell::OpenExternalUrl(std::string_view url) const {
  if (url.empty()) {
    return false;
  }
  if (external_url_opener_) {
    return external_url_opener_(url);
  }
  return SDL_OpenURL(std::string(url).c_str());
}

void WorkspaceShell::SetBottomPanelScrollRow(int scroll_row,
                                             std::size_t line_count,
                                             int visible_rows) {
  const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
  const int clamped_scroll = ClampScrollRowToContent(scroll_row, line_count, visible_rows);
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->scroll_row = clamped_scroll;
    terminal_tab->follow_tail = clamped_scroll >= max_scroll;
  }
}

void WorkspaceShell::ClearTerminalSelection() {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->mouse_selecting = false;
    terminal_tab->selection_anchor.reset();
    terminal_tab->selection_head.reset();
  }
}

void WorkspaceShell::AppendTerminalPendingInput(std::string_view input) {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->pending_input.append(input);
  }
}

void WorkspaceShell::EraseLastTerminalPendingInputCodepoint() {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    EraseLastUtf8Codepoint(terminal_tab->pending_input);
  }
}

void WorkspaceShell::SubmitTerminalPendingInput() {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  const std::size_t cursor_row = terminal_tab->session.cursor_row();
  const std::size_t cursor_column = terminal_tab->session.cursor_column();
  const std::size_t start_row =
      FindWrappedInvocationStartRow(terminal_tab->session, cursor_row);
  const auto lines =
      terminal_tab->session.SnapshotLineRange(start_row, cursor_row - start_row + 1);
  const CapturedTerminalInvocation captured =
      CaptureVisibleTerminalInvocation(lines, cursor_row - start_row, cursor_column);
  terminal_tab->last_command_start_row = start_row + captured.start_row;
  terminal_tab->last_command_invocation = captured.text;
  terminal_tab->last_command_prompt_prefix.clear();
  terminal_tab->has_last_command = !captured.text.empty();
  if (!terminal_tab->pending_input.empty() &&
      captured.text.size() >= terminal_tab->pending_input.size() &&
      captured.text.ends_with(terminal_tab->pending_input)) {
    terminal_tab->last_command_prompt_prefix = captured.text.substr(
        0, captured.text.size() - terminal_tab->pending_input.size());
  }
  terminal_tab->pending_input.clear();
}

std::optional<std::string> WorkspaceShell::LastTerminalCommandText() const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->has_last_command ||
      terminal_tab->last_command_invocation.empty()) {
    return std::nullopt;
  }

  if (terminal_tab->session.using_alternate_screen()) {
    return terminal_tab->last_command_invocation;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  if (terminal_tab->last_command_start_row >= line_count) {
    return terminal_tab->last_command_invocation;
  }
  const auto lines = terminal_tab->session.SnapshotLineRange(
      terminal_tab->last_command_start_row, line_count - terminal_tab->last_command_start_row);
  if (lines.empty()) {
    return terminal_tab->last_command_invocation;
  }

  std::vector<std::string> rows;
  rows.reserve(lines.size());
  for (const auto& line : lines) {
    rows.push_back(TerminalLineText(line));
  }

  std::size_t end_row = rows.size();
  while (end_row > 0 && rows[end_row - 1].empty()) {
    --end_row;
  }
  if (end_row == 0) {
    return terminal_tab->last_command_invocation;
  }

  const std::string prompt_prefix = TrimTrailingTerminalBlanks(terminal_tab->last_command_prompt_prefix);
  const std::string invocation_first_line = FirstLine(terminal_tab->last_command_invocation);
  const std::string& last_line = rows[end_row - 1];
  if ((!prompt_prefix.empty() &&
       (last_line == prompt_prefix || last_line.starts_with(prompt_prefix))) ||
      (!last_line.empty() && invocation_first_line.size() > last_line.size() &&
       invocation_first_line.starts_with(last_line))) {
    --end_row;
    while (end_row > 0 && rows[end_row - 1].empty()) {
      --end_row;
    }
  }

  if (end_row == 0) {
    return terminal_tab->last_command_invocation;
  }

  std::string transcript;
  for (std::size_t row = 0; row < end_row; ++row) {
    if (!transcript.empty()) {
      transcript.push_back('\n');
    }
    transcript += rows[row];
  }

  return transcript.empty() ? std::optional<std::string>(terminal_tab->last_command_invocation)
                            : std::optional<std::string>(std::move(transcript));
}

bool WorkspaceShell::TerminalHasSelection() const {
  const auto selection = ActiveTerminalSelectionBounds();
  return selection.has_value() &&
         (selection->start.row != selection->end.row ||
          selection->start.column != selection->end.column);
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalSelectionPositionForPoint(
    int x,
    int y,
    const std::vector<terminal::TerminalLine>& lines,
    std::size_t first_row) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr || lines.empty()) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const BottomPanelLogLayout panel_layout = ComputeBottomPanelLogLayout(layout, lines.size());
  if (panel_layout.line_height <= 0.0f || y < panel_layout.text_y ||
      y >= panel_layout.content_rect.y + panel_layout.content_rect.h) {
    return std::nullopt;
  }

  const int local_row =
      static_cast<int>((static_cast<float>(y) - panel_layout.text_y) / panel_layout.line_height);
  if (local_row < 0 || local_row >= panel_layout.scroll.visible_rows) {
    return std::nullopt;
  }

  const std::size_t row =
      std::min<std::size_t>(first_row + static_cast<std::size_t>(local_row),
                            first_row + lines.size() - 1);
  const float local_x = std::max(0.0f, static_cast<float>(x) - panel_layout.text_x);
  const std::size_t column = static_cast<std::size_t>(
      std::max(0L, std::lround(local_x / std::max(1.0f, text_renderer_.CharWidth()))));
  return TerminalSelectionPosition{
      .row = row,
      .column = std::min(column, lines[row - first_row].cells.size()),
  };
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalViewportPositionForPoint(int x, int y) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr) {
    return std::nullopt;
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }

  const std::size_t rows = terminal_tab->session.rows();
  const std::size_t columns = terminal_tab->session.columns();
  if (rows == 0 || columns == 0) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const SDL_FRect panel_content = BottomPanelContentRect(layout, panel_state_.command_mode);
  if (!Contains(panel_content, x, y)) {
    return std::nullopt;
  }

  const float text_x = panel_content.x + 12.0f;
  const float text_y = panel_content.y + 8.0f;
  const float line_height = text_renderer_.LineHeight();
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  if (line_height <= 0.0f || y < text_y) {
    return std::nullopt;
  }

  const std::size_t row = static_cast<std::size_t>(
      std::max(0.0f, static_cast<float>(std::floor((static_cast<float>(y) - text_y) / line_height))));
  if (row >= rows) {
    return std::nullopt;
  }

  const float local_x = std::max(0.0f, static_cast<float>(x) - text_x);
  const std::size_t column = static_cast<std::size_t>(std::floor(local_x / char_width));
  return TerminalSelectionPosition{
      .row = row,
      .column = std::min(column, columns - 1),
  };
}

terminal::TerminalSession::MouseButton WorkspaceShell::TerminalMouseButtonForSdl(
    Uint8 button) const {
  return TerminalMouseButtonFromSdl(button);
}

std::optional<TerminalSelectionBounds> WorkspaceShell::ActiveTerminalSelectionBounds() const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return std::nullopt;
  }

  return NormalizeTerminalSelection(
      TerminalSelectionPoint{
          .row = terminal_tab->selection_anchor->row,
          .column = terminal_tab->selection_anchor->column,
      },
      TerminalSelectionPoint{
          .row = terminal_tab->selection_head->row,
          .column = terminal_tab->selection_head->column,
      });
}

std::string WorkspaceShell::SelectedTerminalText() const {
  const auto selection = ActiveTerminalSelectionBounds();
  if (!selection.has_value()) {
    return {};
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return {};
  }

  const std::size_t first_row = selection->start.row;
  const auto lines = terminal_tab->session.SnapshotLineRange(
      first_row, selection->end.row - first_row + 1);
  if (lines.empty()) {
    return {};
  }

  TerminalSelectionBounds rebased = *selection;
  rebased.start.row -= first_row;
  rebased.end.row -= first_row;
  return ExtractTerminalSelectionText(lines, rebased);
}

bool WorkspaceShell::TerminalCellSelected(std::size_t row, std::size_t column) const {
  const std::optional<TerminalSelectionBounds> selection = ActiveTerminalSelectionBounds();
  if (!selection.has_value()) {
    return false;
  }

  return TerminalSelectionContainsCell(*selection, row, column);
}

}  // namespace microide::workspace
