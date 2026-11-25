#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalInput.h"

namespace microide::terminal {

class TerminalPredictionOverlay {
 public:
  enum class Mode : std::uint8_t { Adaptive, Always, Never };
  static constexpr std::chrono::milliseconds kAdaptiveThreshold{30};
  static constexpr std::size_t kMaxPending = 256;

  struct View {
    std::deque<TerminalLine>& lines;
    std::size_t& cursor_row;
    std::size_t& cursor_column;
    std::size_t columns = 80;
    bool cursor_visible = true;
    bool alternate_screen = false;
  };

  static Mode ParseMode(std::string_view text) {
    return text == "always" ? Mode::Always : text == "never" ? Mode::Never : Mode::Adaptive;
  }

  void set_mode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  void set_round_trip(std::optional<std::chrono::milliseconds> rtt) { rtt_ = rtt; }

  bool Typed(const TerminalInputEvent& event, View view);
  bool Withdraw(View view);
  bool Judge(std::uint64_t echo_ack, View view);
  void Clear();

  std::size_t pending() const { return pending_.size(); }
  bool suppressed() const { return suppressed_; }
  bool drawing() const;

 private:
  struct Prediction {
    std::uint64_t seq = 0;
    bool erase = false;
    TerminalCell glyph;
    std::size_t column = 0;
    std::size_t row_from_end = 0;
    bool placed = false;
  };

  bool Place(View view);

  Mode mode_ = Mode::Adaptive;
  std::optional<std::chrono::milliseconds> rtt_;
  std::deque<Prediction> pending_;
  bool suppressed_ = false;
  std::uint64_t blocked_until_ = 0;
  bool drawn_ = false;
  std::size_t saved_row_ = 0;
  TerminalLine saved_line_;
  std::size_t saved_cursor_column_ = 0;
};

}
