#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

void WorkspaceShell::OpenTerminal(std::string command, bool focus_terminal, bool log_feedback) {
  if (project_root_.empty()) {
    if (log_feedback) {
      LogMessage("No project is loaded");
    }
    return;
  }
  const std::filesystem::path working_directory = project_root_;
  auto terminal_tab = std::make_unique<TerminalTabState>();
  if (terminal_event_type_ != 0) {
    terminal_tab->session.SetWakeEventType(terminal_event_type_);
  }
  if (!terminal_tab->session.Start(working_directory, command)) {
    if (log_feedback) {
      LogMessage("Failed to start terminal");
    }
    return;
  }

  terminal_tabs_.push_back(std::move(terminal_tab));
  active_terminal_tab_index_ = terminal_tabs_.size() - 1;
  if (focus_terminal) {
    focus_ = FocusTarget::Panel;
  }
  if (log_feedback) {
    if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr) {
      LogMessage("Terminal started: " + active_terminal->session.LaunchLabel());
    }
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
