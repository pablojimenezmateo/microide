#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "terminal/TerminalHostChannel.h"

namespace microide::workspace {

// The terminal panel as a session remembers it: which tabs, how each is split,
// and where each pane's shell was. Shared by the session format (what is
// written) and the project state (what is waiting to be revived).
//
// A LOCAL pane revives as VS Code's do when a window comes back: a fresh shell in
// the same layout and directory; its process and scrollback are not kept. A HOST
// pane (a remote project's) IS kept: quit and close-project leave its shell
// running on the host (dev-docs/design/remote-projects.md § 6.12), and the record
// names it -- the server process and its handle there -- so a reopen reattaches
// to the same shell, screen and history. When that server is gone or no longer
// has it, the pane starts fresh in `cwd`, as a local one does.
struct TerminalPaneRecord {
  std::filesystem::path cwd;  // in the editor's tree; empty = the project root
  std::string command;        // empty = the configured shell
  terminal::HostIdentity host;  // empty = a fresh shell (a local pane, an older session)
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
// The hello's own cap on a daemon epoch: a longer one names no server.
inline constexpr std::size_t kMaxRecordedHostEpochBytes = 64;

}  // namespace microide::workspace
