#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "terminal/TerminalHostChannel.h"

namespace microide::workspace {

struct TerminalPaneRecord {
  std::filesystem::path cwd;
  std::string command;
  terminal::HostIdentity host;
};

struct TerminalTabRecord {
  std::vector<TerminalPaneRecord> panes;
  std::vector<float> weights;
  std::size_t active_pane = 0;
};

struct TerminalLayoutRecord {
  std::vector<TerminalTabRecord> tabs;
  std::size_t active_tab = 0;

  [[nodiscard]] bool empty() const { return tabs.empty(); }
};

inline constexpr std::size_t kMaxRevivedTerminalTabs = 16;
inline constexpr std::size_t kMaxRecordedHostEpochBytes = 64;

}
