#include "workspace/WorkspaceShell.h"

#include <utility>

#include "workspace/TerminalPanelService.h"

namespace microide::workspace {

TerminalPanelService WorkspaceShell::MakeTerminalPanelService() {
  return TerminalPanelService(TerminalPanelService::Operations{
      .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
      .clear_terminal_selection = [this]() { ClearTerminalSelection(); },
      .append_terminal_pending_input =
          [this](std::string_view input) { AppendTerminalPendingInput(input); },
      .terminal_url_at_point = [this](float x, float y) { return TerminalUrlAtPoint(x, y); },
      .open_external_url = [this](std::string_view url) { return OpenExternalUrl(url); },
      .sync_primary_selection_with_terminal_selection =
          [this]() { SyncPrimarySelectionWithTerminalSelection(); },
      .open_terminal =
          [this](std::string command, bool focus_terminal, bool log_feedback) {
            OpenTerminal(std::move(command), focus_terminal, log_feedback);
          },
      .close_terminal_tab = [this](std::size_t index) { CloseTerminalTab(index); },
      .move_active_terminal_tab_to =
          [this](std::size_t index) { return MoveActiveTerminalTabTo(index); },
  });
}

}  // namespace microide::workspace
