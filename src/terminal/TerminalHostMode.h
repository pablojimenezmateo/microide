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
#include "terminal/TerminalHostWire.h"

namespace microide::terminal {

struct TerminalHostRequests {
  std::optional<std::string> open_url;
  std::optional<TerminalHostEdit> edit;
};

struct TerminalHostMode {
  std::shared_ptr<TerminalHostChannel> channel;
  std::uint64_t next_input_seq = 1;
  std::uint64_t screen_top = 0;
  std::size_t screen_lines = 0;
  bool alternate = false;
  bool mirrored = false;
  std::optional<std::deque<TerminalLine>> primary_stash;
  std::size_t primary_stash_screen_lines = 0;
  TerminalHostHistory history;

  struct PendingStart {
    std::filesystem::path working_directory;
    std::string command;
    std::vector<std::string> shell_argv;
    HostIdentity attach;
  };
  std::optional<PendingStart> pending;
  HostIdentity detached_identity;
  bool link_down = false;
  TerminalHostRequests requests;
  bool announce_reattach = false;
};

}
