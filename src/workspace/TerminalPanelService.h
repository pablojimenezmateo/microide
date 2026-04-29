#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

class TerminalPanelService {
 public:
  struct Operations {
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<std::optional<std::string>(float, float)> terminal_url_at_point;
    std::function<bool(std::string_view)> open_external_url;
    std::function<void()> sync_primary_selection_with_terminal_selection;
    std::function<void(std::string, bool, bool)> open_terminal;
    std::function<void(std::size_t)> close_terminal_tab;
    std::function<bool(std::size_t)> move_active_terminal_tab_to;
  };

  explicit TerminalPanelService(Operations operations);

  std::optional<std::string> ReadPrimarySelectionText() const;
  void ClearTerminalSelection();
  void AppendTerminalPendingInput(std::string_view input);
  std::optional<std::string> TerminalUrlAtPoint(float x, float y) const;
  bool OpenExternalUrl(std::string_view url) const;
  void SyncPrimarySelectionWithTerminalSelection();
  void OpenTerminal(std::string command = {}, bool focus_terminal = true, bool log_feedback = false);
  void CloseTerminalTab(std::size_t index);
 bool MoveActiveTerminalTabTo(std::size_t index);

 private:
  Operations operations_;
};

}  // namespace microide::workspace
