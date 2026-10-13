#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

// The terminal panel as a session remembers it: which tabs, how each is split,
// and where each pane's shell was. VS Code revives terminals the same way when
// a window comes back -- fresh shells in the same layout and directories; the
// processes and their scrollback are not kept. Shared by the session format
// (what is written) and the project state (what is waiting to be revived).
struct TerminalPaneRecord {
  std::filesystem::path cwd;  // in the editor's tree; empty = the project root
  std::string command;        // empty = the configured shell
};

struct TerminalTabRecord {
  std::vector<TerminalPaneRecord> panes;
  std::vector<float> weights;  // parallel to panes; anything invalid restores even
  std::size_t active_pane = 0;
};

struct TerminalLayoutRecord {
  std::vector<TerminalTabRecord> tabs;
  std::size_t active_tab = 0;

  [[nodiscard]] bool empty() const { return tabs.empty(); }
};

// Decode bounds: a forged session must not revive a fork bomb.
inline constexpr std::size_t kMaxRevivedTerminalTabs = 16;

}  // namespace microide::workspace
