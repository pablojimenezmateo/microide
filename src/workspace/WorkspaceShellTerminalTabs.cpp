#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "util/Parse.h"
#include "workspace/SettingFlags.h"
#include "workspace/TabReorder.h"

namespace microide::workspace {

std::size_t WorkspaceShell::TerminalScrollbackLines() const {
  const int parsed = util::ParseIntOr(GetSettingValue("terminal.scrollback_lines"), 2000);
  return static_cast<std::size_t>(std::clamp(parsed, 200, 100000));
}

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
  terminal_tab->session.SetMaxScrollbackLines(TerminalScrollbackLines());
  const bool started =
      terminal::UsePlaceholderTerminalsForTesting()
          ? terminal_tab->session.StartPlaceholderForTesting(working_directory, command)
          : terminal_tab->session.Start(working_directory, command,
                                        GetSettingValue("terminal.shell").value_or(""));
  if (!started) {
    return;
  }

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

void WorkspaceShell::OpenDefaultTerminalForProjectInit() {
  if (terminal::UsePlaceholderTerminalsForTesting()) {
    // Test mode: install a bare, unstarted terminal tab — no real shell spawn.
    context_.current_project_state.terminal_tabs.push_back(
        std::make_unique<TerminalTabState>());
    context_.current_project_state.active_terminal_tab_index =
        context_.current_project_state.terminal_tabs.size() - 1;
    context_.current_project_state.panel.content = PanelContentKind::Terminal;
    context_.current_project_state.surface.focus = FocusTarget::Panel;
  } else {
    OpenTerminal({}, true, false);
  }
}

WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() {
  return context_.current_project_state.active_terminal_tab();
}

const WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() const {
  return context_.current_project_state.active_terminal_tab();
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
  if (!ReorderActive(context_.current_project_state.terminal_tabs,
                     context_.current_project_state.active_terminal_tab_index, index)) {
    return false;
  }
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  return true;
}

void WorkspaceShell::CloseTerminalTab(std::size_t index) {
  if (index >= context_.current_project_state.terminal_tabs.size()) {
    return;
  }

  const bool panel_visible_before = BottomPanelVisible();
  context_.current_project_state.terminal_tabs.erase(context_.current_project_state.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (context_.current_project_state.terminal_tabs.empty()) {
    context_.current_project_state.active_terminal_tab_index = 0;
    ClearTerminalSelection();
    if (context_.current_project_state.panel.content == PanelContentKind::Terminal) {
      context_.current_project_state.panel.content = PanelContentKind::None;
    }
    if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
    }
    if (BottomPanelVisible() != panel_visible_before) {
      MarkLayoutDirty();
      RequestWindowRedraw();
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
  const bool had_terminal_tabs = tab_count_before > 0;
  // OSC 52 lets a program running in the terminal set the system clipboard. That
  // is a silent poisoning vector (any output could swap a copied command for a
  // malicious one), so honor it only when the user has opted in. The pending text
  // is still drained either way so it can't accumulate.
  const bool allow_osc52_clipboard =
      SettingFlagEnabled(GetSettingValue("terminal.osc52_clipboard_write"), false);
  for (const auto& terminal_tab : context_.current_project_state.terminal_tabs) {
    if (terminal_tab == nullptr) {
      continue;
    }
    terminal_tab->session.ConsumeWakeEvent();
    const std::optional<std::string> clipboard_text =
        terminal_tab->session.ConsumePendingClipboardText();
    if (clipboard_text.has_value()) {
      if (allow_osc52_clipboard) {
        WriteClipboardText(*clipboard_text);
      } else {
        // Surface every blocked write so it is not a silent regression for users
        // who rely on OSC 52 yank-to-clipboard (tmux/vim over SSH). The toast
        // service coalesces/expires duplicates, so this cannot flood the UI.
        Notify(NotificationService::Tone::Info,
               "A terminal program tried to set the clipboard (OSC 52). Enable "
               "\"Allow Terminal Clipboard Writes (OSC 52)\" in Settings to permit it.");
      }
    }
  }
  ReapExitedTerminalTabs();
  SyncTerminalFocusState();
  if (had_terminal_tabs) {
    const bool reloaded = ReloadProjectIfFilesChanged(false);
    if (!reloaded) {
      RequestAutomaticGitSidebarRefresh();
    }
  }
  if (BottomPanelVisible() != panel_visible_before || context_.current_project_state.terminal_tabs.size() != tab_count_before) {
    if (BottomPanelVisible() != panel_visible_before) {
      MarkLayoutDirty();
    }
    RequestWindowRedraw();
  } else if (panel_visible_before) {
    RequestBottomPanelRedraw();
  }
}

}  // namespace microide::workspace
