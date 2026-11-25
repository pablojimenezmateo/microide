#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

namespace microide::terminal {

class TerminalHostFrameBuilder {
 public:
  void Reset();
  void Resume(std::uint64_t screen_top, bool alternate);

  std::uint64_t capture_from() const { return reset_ ? 0 : primary_top_; }
  std::size_t capture_lines_before_screen(std::size_t prefetch) const {
    return reset_ ? prefetch : std::numeric_limits<std::size_t>::max();
  }

  std::size_t Build(const TerminalSession::HostCapture& capture,
                    std::size_t scrollback_budget_bytes, TerminalHostFrame& frame);

  bool UpToDate(const TerminalSession::HostCapture& capture) const;

  static std::size_t EstimateLineBytes(const TerminalLine& line);

 private:
  bool reset_ = true;
  bool alternate_ = false;
  std::uint64_t primary_top_ = 0;
  std::uint64_t shadow_top_ = 0;
  std::vector<TerminalLine> shadow_;
  std::uint64_t generation_ = 0;
  std::string title_;
  std::string working_directory_;
};

}
