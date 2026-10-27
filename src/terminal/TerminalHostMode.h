#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalHostChannel.h"
#include "terminal/TerminalHostHistory.h"

namespace microide::terminal {

// What a TerminalSession holds in HOST MODE (dev-docs/design/remote-projects.md
// § 6.5, § 6.12) beyond the buffer every session has: the channel to the host
// terminal, where the mirrored screen sits, the history it has fetched, and the
// states around a host terminal's life -- pending before the connection is up,
// detached after quit or close-project. One struct so the session's header stays
// its public surface. Guarded by the session's mutex.
struct TerminalHostMode {
  std::shared_ptr<TerminalHostChannel> channel;
  std::uint64_t next_input_seq = 1;
  std::uint64_t screen_top = 0;   // host-absolute index of the first screen line
  std::size_t screen_lines = 0;   // screen lines at the tail of the session's lines
  bool alternate = false;
  bool mirrored = false;  // a frame has been applied
  // The primary buffer while the host shows its alternate screen. Optional: a
  // libstdc++ deque allocates when default-constructed, and every terminal —
  // local ones, which never stash — paid two allocations for this slot.
  std::optional<std::deque<TerminalLine>> primary_stash;
  std::size_t primary_stash_screen_lines = 0;
  TerminalHostHistory history;

  // A start asked for before the host's connection was up, kept until it is
  // (TerminalSession::StartPendingOnHost).
  struct PendingStart {
    std::filesystem::path working_directory;
    std::string command;
    std::vector<std::string> shell_argv;
    HostIdentity attach;
  };
  std::optional<PendingStart> pending;
  HostIdentity detached_identity;  // DetachFromHost's, until Stop or a new start
  bool link_down = false;          // HostConnectionLost, until a reattach or a frame
  bool announce_reattach = false;  // the next cold frame draws the reattached rule
};

}  // namespace microide::terminal
