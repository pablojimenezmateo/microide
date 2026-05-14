#include "workspace/WorkspaceShell.h"

#include <algorithm>

namespace microide::workspace {

void WorkspaceShell::OpenTerminal(std::string command, bool focus_terminal, bool log_feedback) {
  (void) log_feedback;
  if (context_.current_project_state.root.empty()) {
    return;
  }
  const bool bottom_panel_was_visible = BottomPanelVisible();
  const bool panel_already_showing_terminal =
      context_.current_project_state.panel.content == PanelContentKind::Terminal;
  const std::filesystem::path working_directory = context_.current_project_state.root;
  auto terminal_tab = std::make_unique<TerminalTabState>();
  if (terminal_event_type_ != 0) {
    terminal_tab->session.SetWakeEventType(terminal_event_type_);
  }
#ifdef MICROIDE_TESTING
  if (!terminal_tab->session.StartPlaceholderForTesting(working_directory, command)) {
    return;
  }
#else
  if (!terminal_tab->session.Start(working_directory, command)) {
    return;
  }
#endif

  context_.current_project_state.terminal_tabs.push_back(std::move(terminal_tab));
  context_.current_project_state.active_terminal_tab_index = context_.current_project_state.terminal_tabs.size() - 1;
  if (focus_terminal || panel_already_showing_terminal) {
    context_.current_project_state.panel.content = PanelContentKind::Terminal;
  }
  if (focus_terminal) {
    context_.current_project_state.surface.focus = FocusTarget::Panel;
  }

  MarkLayoutDirty();
  if (BottomPanelVisible() != bottom_panel_was_visible) {
    RequestWindowRedraw();
  } else {
    RequestBottomPanelRedraw();
  }
}

WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() {
  if (context_.current_project_state.active_terminal_tab_index >= context_.current_project_state.terminal_tabs.size()) {
    return nullptr;
  }
  return context_.current_project_state.terminal_tabs[context_.current_project_state.active_terminal_tab_index].get();
}

const WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() const {
  if (context_.current_project_state.active_terminal_tab_index >= context_.current_project_state.terminal_tabs.size()) {
    return nullptr;
  }
  return context_.current_project_state.terminal_tabs[context_.current_project_state.active_terminal_tab_index].get();
}

std::optional<std::size_t> WorkspaceShell::FocusedTerminalTabIndex() const {
  if (!context_.interaction_state.window_has_input_focus ||
      CurrentTextInputSurface() != TextInputSurface::Terminal ||
      context_.current_project_state.active_terminal_tab_index >= context_.current_project_state.terminal_tabs.size() ||
      context_.current_project_state.terminal_tabs[context_.current_project_state.active_terminal_tab_index] == nullptr) {
    return std::nullopt;
  }
  return context_.current_project_state.active_terminal_tab_index;
}

void WorkspaceShell::SyncTerminalFocusState() {
  const std::optional<std::size_t> focused_index = FocusedTerminalTabIndex();
  for (std::size_t index = 0; index < context_.current_project_state.terminal_tabs.size(); ++index) {
    auto* terminal_tab = context_.current_project_state.terminal_tabs[index].get();
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
  if (context_.current_project_state.active_terminal_tab_index >= context_.current_project_state.terminal_tabs.size() || index >= context_.current_project_state.terminal_tabs.size()) {
    return false;
  }
  if (context_.current_project_state.active_terminal_tab_index == index) {
    return true;
  }

  std::unique_ptr<TerminalTabState> moved_tab =
      std::move(context_.current_project_state.terminal_tabs[context_.current_project_state.active_terminal_tab_index]);
  context_.current_project_state.terminal_tabs.erase(
      context_.current_project_state.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(context_.current_project_state.active_terminal_tab_index));
  context_.current_project_state.terminal_tabs.insert(context_.current_project_state.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(index),
                        std::move(moved_tab));
  context_.current_project_state.active_terminal_tab_index = index;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  return true;
}

void WorkspaceShell::CloseTerminalTab(std::size_t index) {
  if (index >= context_.current_project_state.terminal_tabs.size()) {
    return;
  }

  context_.current_project_state.terminal_tabs.erase(context_.current_project_state.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (context_.current_project_state.terminal_tabs.empty()) {
    context_.current_project_state.active_terminal_tab_index = 0;
    ClearTerminalSelection();
    if (!context_.current_project_state.panel.command_mode &&
        context_.current_project_state.panel.content == PanelContentKind::Terminal) {
      context_.current_project_state.panel.content = PanelContentKind::None;
    }
    if (context_.current_project_state.surface.focus == FocusTarget::Panel && !context_.current_project_state.panel.command_mode) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
    }
    return;
  }

  context_.current_project_state.active_terminal_tab_index =
      std::min(context_.current_project_state.active_terminal_tab_index > index ? context_.current_project_state.active_terminal_tab_index - 1
                                                  : context_.current_project_state.active_terminal_tab_index,
               context_.current_project_state.terminal_tabs.size() - 1);
}

void WorkspaceShell::ReapExitedTerminalTabs() {
  for (std::size_t i = 0; i < context_.current_project_state.terminal_tabs.size();) {
    if (context_.current_project_state.terminal_tabs[i] != nullptr && !context_.current_project_state.terminal_tabs[i]->session.running()) {
      CloseTerminalTab(i);
      continue;
    }
    ++i;
  }
}

void WorkspaceShell::ConsumeTerminalSessionUpdates() {
  const bool panel_visible_before = BottomPanelVisible();
  const std::size_t tab_count_before = context_.current_project_state.terminal_tabs.size();
  for (const auto& terminal_tab : context_.current_project_state.terminal_tabs) {
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
  if (BottomPanelVisible() != panel_visible_before || context_.current_project_state.terminal_tabs.size() != tab_count_before) {
    RequestWindowRedraw();
  } else if (panel_visible_before) {
    RequestBottomPanelRedraw();
  }
}

}  // namespace microide::workspace
