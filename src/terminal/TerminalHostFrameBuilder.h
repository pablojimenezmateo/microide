#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

namespace microide::terminal {

// The host's model of what ONE client holds of one terminal, and the frame that
// brings it up to date (TerminalHostWire.h describes the coordinates). Pure: no
// I/O, no clock; the server's terminal table owns one per attached client.
class TerminalHostFrameBuilder {
 public:
  // The next frame is a cold attach (kReset): the client holds nothing.
  void Reset();

  // The capture window for the next frame: lines from `capture_from()`, at most
  // `capture_lines_before_screen(prefetch)` of them above the screen. A cold
  // attach starts `prefetch` lines up (remote.scrollback_prefetch_lines); older
  // history is fetched on demand.
  std::uint64_t capture_from() const { return reset_ ? 0 : primary_top_; }
  std::size_t capture_lines_before_screen(std::size_t prefetch) const {
    return reset_ ? prefetch : std::numeric_limits<std::size_t>::max();
  }

  // Build the frame from `capture` into `frame`. Scrollback lines beyond
  // `scrollback_budget_bytes` (estimated encoded size) are withheld as a Gap.
  // Returns the estimated bytes of scrollback lines sent (what the client acks).
  // `frame.echo_ack` is the caller's.
  std::size_t Build(const TerminalSession::HostCapture& capture,
                    std::size_t scrollback_budget_bytes, TerminalHostFrame& frame);

  // Whether a frame built now would carry nothing new for this capture's state:
  // same generation and no pending outward signal.
  bool UpToDate(const TerminalSession::HostCapture& capture) const;

  static std::size_t EstimateLineBytes(const TerminalLine& line);

 private:
  bool reset_ = true;
  bool alternate_ = false;
  std::uint64_t primary_top_ = 0;
  // The client's current screen: lines from `shadow_top_` (absolute; 0 on the
  // alternate screen).
  std::uint64_t shadow_top_ = 0;
  std::vector<TerminalLine> shadow_;
  std::uint64_t generation_ = 0;
  std::string title_;
  std::string working_directory_;
};

}  // namespace microide::terminal
