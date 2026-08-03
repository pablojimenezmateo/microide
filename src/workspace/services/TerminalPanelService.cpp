#include "workspace/services/TerminalPanelService.h"

#include <utility>

namespace microide::workspace {

TerminalPanelService::TerminalPanelService(Operations operations)
    : operations_(std::move(operations)) {}

std::optional<std::string> TerminalPanelService::ReadPrimarySelectionText() const {
  return operations_.read_primary_selection_text();
}

void TerminalPanelService::ClearTerminalSelection() {
  operations_.clear_terminal_selection();
}

void TerminalPanelService::AppendTerminalPendingInput(std::string_view input) {
  operations_.append_terminal_pending_input(input);
}

std::optional<std::string> TerminalPanelService::TerminalUrlAtPoint(float x, float y) const {
  return operations_.terminal_url_at_point(x, y);
}

bool TerminalPanelService::OpenExternalUrl(std::string_view url) const {
  return operations_.open_external_url(url);
}

void TerminalPanelService::SyncPrimarySelectionWithTerminalSelection() {
  operations_.sync_primary_selection_with_terminal_selection();
}

void TerminalPanelService::OpenTerminal(std::string command,
                                        bool focus_terminal,
                                        bool log_feedback) {
  operations_.open_terminal(std::move(command), focus_terminal, log_feedback);
}

void TerminalPanelService::CloseTerminalTab(std::size_t index) {
  operations_.close_terminal_tab(index);
}

bool TerminalPanelService::MoveActiveTerminalTabTo(std::size_t index) {
  return operations_.move_active_terminal_tab_to(index);
}

}  // namespace microide::workspace
