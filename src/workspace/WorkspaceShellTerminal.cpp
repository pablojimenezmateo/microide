#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

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
    const char character = line.cells[column].character;
    text.push_back(character == '\0' ? ' ' : character);
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

}  // namespace

void WorkspaceShell::OpenTerminal(std::string command, bool focus_terminal, bool log_feedback) {
  (void) log_feedback;
  if (project_root_.empty()) {
    return;
  }
  const std::filesystem::path working_directory = project_root_;
  auto terminal_tab = std::make_unique<TerminalTabState>();
  if (terminal_event_type_ != 0) {
    terminal_tab->session.SetWakeEventType(terminal_event_type_);
  }
  if (!terminal_tab->session.Start(working_directory, command)) {
    return;
  }

  terminal_tabs_.push_back(std::move(terminal_tab));
  active_terminal_tab_index_ = terminal_tabs_.size() - 1;
  if (focus_terminal) {
    focus_ = FocusTarget::Panel;
  }
}

WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() {
  if (active_terminal_tab_index_ >= terminal_tabs_.size()) {
    return nullptr;
  }
  return terminal_tabs_[active_terminal_tab_index_].get();
}

const WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() const {
  if (active_terminal_tab_index_ >= terminal_tabs_.size()) {
    return nullptr;
  }
  return terminal_tabs_[active_terminal_tab_index_].get();
}

std::optional<std::size_t> WorkspaceShell::FocusedTerminalTabIndex() const {
  if (!window_has_input_focus_ || CurrentTextInputSurface() != TextInputSurface::Terminal ||
      active_terminal_tab_index_ >= terminal_tabs_.size() ||
      terminal_tabs_[active_terminal_tab_index_] == nullptr) {
    return std::nullopt;
  }
  return active_terminal_tab_index_;
}

void WorkspaceShell::SyncTerminalFocusState() {
  const std::optional<std::size_t> focused_index = FocusedTerminalTabIndex();
  for (std::size_t index = 0; index < terminal_tabs_.size(); ++index) {
    auto* terminal_tab = terminal_tabs_[index].get();
    if (terminal_tab == nullptr) {
      continue;
    }

    const bool should_focus = focused_index.has_value() && *focused_index == index &&
                              terminal_tab->session.WantsFocusEvents();
    if (terminal_tab->focus_events_active == should_focus) {
      continue;
    }

    terminal_tab->session.SendFocusEvent(should_focus);
    terminal_tab->focus_events_active = should_focus;
  }
}

bool WorkspaceShell::MoveActiveTerminalTabTo(std::size_t index) {
  if (active_terminal_tab_index_ >= terminal_tabs_.size() || index >= terminal_tabs_.size()) {
    return false;
  }
  if (active_terminal_tab_index_ == index) {
    return true;
  }

  std::unique_ptr<TerminalTabState> moved_tab =
      std::move(terminal_tabs_[active_terminal_tab_index_]);
  terminal_tabs_.erase(terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(active_terminal_tab_index_));
  terminal_tabs_.insert(terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(index),
                        std::move(moved_tab));
  active_terminal_tab_index_ = index;
  focus_ = FocusTarget::Panel;
  return true;
}

void WorkspaceShell::CloseTerminalTab(std::size_t index) {
  if (index >= terminal_tabs_.size()) {
    return;
  }

  terminal_tabs_.erase(terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(index));
  if (terminal_tabs_.empty()) {
    active_terminal_tab_index_ = 0;
    ClearTerminalSelection();
    if (focus_ == FocusTarget::Panel && !command_mode_) {
      focus_ = FocusTarget::Editor;
    }
    return;
  }

  active_terminal_tab_index_ =
      std::min(active_terminal_tab_index_ > index ? active_terminal_tab_index_ - 1
                                                  : active_terminal_tab_index_,
               terminal_tabs_.size() - 1);
}

void WorkspaceShell::ReapExitedTerminalTabs() {
  for (std::size_t i = 0; i < terminal_tabs_.size();) {
    if (terminal_tabs_[i] != nullptr && !terminal_tabs_[i]->session.running()) {
      CloseTerminalTab(i);
      continue;
    }
    ++i;
  }
}

void WorkspaceShell::ConsumeTerminalSessionUpdates() {
  for (const auto& terminal_tab : terminal_tabs_) {
    if (terminal_tab == nullptr) {
      continue;
    }
    const std::optional<std::string> clipboard_text =
        terminal_tab->session.ConsumePendingClipboardText();
    if (clipboard_text.has_value()) {
      WriteClipboardText(*clipboard_text);
    }
  }
  ReapExitedTerminalTabs();
  SyncTerminalFocusState();
}

bool WorkspaceShell::BottomPanelVisible() const {
  return command_mode_ || !terminal_tabs_.empty();
}

int WorkspaceShell::BottomPanelVisibleRows(float panel_height) const {
  return BottomPanelVisibleRowsForHeight(panel_height, text_renderer_.LineHeight(), command_mode_);
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

  const auto lines = terminal_tab->session.SnapshotLines();
  const CapturedTerminalInvocation captured =
      CaptureVisibleTerminalInvocation(lines,
                                      terminal_tab->session.cursor_row(),
                                      terminal_tab->session.cursor_column());
  terminal_tab->last_command_start_row = captured.start_row;
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

  const auto lines = terminal_tab->session.SnapshotLines();
  if (lines.empty() || terminal_tab->last_command_start_row >= lines.size()) {
    return terminal_tab->last_command_invocation;
  }

  std::vector<std::string> rows;
  rows.reserve(lines.size() - terminal_tab->last_command_start_row);
  for (std::size_t row = terminal_tab->last_command_start_row; row < lines.size(); ++row) {
    rows.push_back(TerminalLineText(lines[row]));
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
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return false;
  }
  const auto selection = NormalizeTerminalSelection(
      TerminalSelectionPoint{
          .row = terminal_tab->selection_anchor->row,
          .column = terminal_tab->selection_anchor->column,
      },
      TerminalSelectionPoint{
          .row = terminal_tab->selection_head->row,
          .column = terminal_tab->selection_head->column,
      });
  return selection.has_value() &&
         (selection->start.row != selection->end.row ||
          selection->start.column != selection->end.column);
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalSelectionPositionForPoint(
    int x,
    int y,
    const std::vector<terminal::TerminalLine>& lines) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr || lines.empty() ||
      last_window_width_ <= 0 || last_window_height_ <= 0) {
    return std::nullopt;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
  const float text_x = panel_content.x + 12.0f;
  const float text_y = panel_content.y + 8.0f;
  const float line_height = text_renderer_.LineHeight();
  if (line_height <= 0.0f || y < text_y || y >= panel_content.y + panel_content.h) {
    return std::nullopt;
  }

  const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
  const int scroll_row = BottomPanelScrollRow(lines.size(), visible_rows);
  const int local_row = static_cast<int>((static_cast<float>(y) - text_y) / line_height);
  if (local_row < 0 || local_row >= visible_rows) {
    return std::nullopt;
  }

  const std::size_t row = std::min<std::size_t>(static_cast<std::size_t>(scroll_row + local_row),
                                                lines.size() - 1);
  const float local_x = std::max(0.0f, static_cast<float>(x) - text_x);
  const std::size_t column = static_cast<std::size_t>(
      std::max(0L, std::lround(local_x / std::max(1.0f, text_renderer_.CharWidth()))));
  return TerminalSelectionPosition{
      .row = row,
      .column = std::min(column, lines[row].cells.size()),
  };
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalViewportPositionForPoint(int x, int y) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr || last_window_width_ <= 0 ||
      last_window_height_ <= 0) {
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

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
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

std::string WorkspaceShell::SelectedTerminalText(
    const std::vector<terminal::TerminalLine>& lines) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return {};
  }

  const std::optional<TerminalSelectionBounds> selection = NormalizeTerminalSelection(
      TerminalSelectionPoint{
          .row = terminal_tab->selection_anchor->row,
          .column = terminal_tab->selection_anchor->column,
      },
      TerminalSelectionPoint{
          .row = terminal_tab->selection_head->row,
          .column = terminal_tab->selection_head->column,
      });
  if (!selection.has_value()) {
    return {};
  }

  return ExtractTerminalSelectionText(lines, *selection);
}

bool WorkspaceShell::TerminalCellSelected(std::size_t row, std::size_t column) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return false;
  }

  const std::optional<TerminalSelectionBounds> selection = NormalizeTerminalSelection(
      TerminalSelectionPoint{
          .row = terminal_tab->selection_anchor->row,
          .column = terminal_tab->selection_anchor->column,
      },
      TerminalSelectionPoint{
          .row = terminal_tab->selection_head->row,
          .column = terminal_tab->selection_head->column,
      });
  if (!selection.has_value()) {
    return false;
  }

  return TerminalSelectionContainsCell(*selection, row, column);
}

}  // namespace microide::workspace
