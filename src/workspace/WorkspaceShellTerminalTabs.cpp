#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

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
    surface_.focus = FocusTarget::Panel;
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
  if (!interaction_state_.window_has_input_focus ||
      CurrentTextInputSurface() != TextInputSurface::Terminal ||
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
  terminal_tabs_.erase(
      terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(active_terminal_tab_index_));
  terminal_tabs_.insert(terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(index),
                        std::move(moved_tab));
  active_terminal_tab_index_ = index;
  surface_.focus = FocusTarget::Panel;
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
    if (surface_.focus == FocusTarget::Panel && !panel_state_.command_mode) {
      surface_.focus = FocusTarget::Editor;
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
  const bool panel_visible_before = BottomPanelVisible();
  const std::size_t tab_count_before = terminal_tabs_.size();
  for (const auto& terminal_tab : terminal_tabs_) {
    if (terminal_tab == nullptr) {
      continue;
    }
    terminal_tab->session.ConsumeWakeEvent();
    const std::optional<std::string> clipboard_text =
        terminal_tab->session.ConsumePendingClipboardText();
    if (clipboard_text.has_value()) {
      WriteClipboardText(*clipboard_text);
    }
  }
  ReapExitedTerminalTabs();
  SyncTerminalFocusState();
  if (BottomPanelVisible() != panel_visible_before || terminal_tabs_.size() != tab_count_before) {
    RequestWindowRedraw();
  } else if (panel_visible_before) {
    RequestBottomPanelRedraw();
  }
}

}  // namespace microide::workspace
