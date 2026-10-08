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

// Local echo prediction for a host terminal, mosh-style
// (dev-docs/design/remote-projects.md § 6.5): a typed printable character is drawn
// at the cursor — underlined — in the keystroke's own frame, and a backspace
// erases one, instead of waiting a round trip for the host's echo.
//
// An OVERLAY, not a second terminal model: it draws over the confirmed lines the
// session mirrors from the host, and it is taken back out (Withdraw) before every
// host frame is applied, because a frame's Keep/Promote rows are only valid
// against the confirmed content. After the frame, every prediction its
// `echo_ack` covers is judged against the cell where it was drawn: confirmed when
// the host drew the same glyph there, contradicted otherwise. A contradiction
// drops every prediction and stops DRAWING until a later prediction is confirmed
// (they are still tracked, so the overlay learns when the program echoes again).
// What is still pending is re-placed from the new confirmed cursor.
//
// It predicts nothing for control keys (and nothing after one until the host has
// answered it: the cursor's position is then unknown), nothing at the right
// margin, nothing while the cursor is hidden, and nothing on the alternate screen.
// `Adaptive` draws only while the round trip exceeds 30 ms, so on a LAN nobody
// ever sees it.
//
// Pure: no I/O, no clock of its own; the round trip is an input.
class TerminalPredictionOverlay {
 public:
  enum class Mode : std::uint8_t { Adaptive, Always, Never };
  static constexpr std::chrono::milliseconds kAdaptiveThreshold{30};
  static constexpr std::size_t kMaxPending = 256;

  // The confirmed state the overlay draws over (the session's own members).
  struct View {
    std::deque<TerminalLine>& lines;
    std::size_t& cursor_row;
    std::size_t& cursor_column;
    std::size_t columns = 80;
    bool cursor_visible = true;
    bool alternate_screen = false;
  };

  // `remote.predict`'s spelling; anything unrecognized is the default, Adaptive.
  static Mode ParseMode(std::string_view text) {
    return text == "always" ? Mode::Always : text == "never" ? Mode::Never : Mode::Adaptive;
  }

  void set_mode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  void set_round_trip(std::optional<std::chrono::milliseconds> rtt) { rtt_ = rtt; }

  // The user typed `event` (its seq assigned). True when the view changed.
  bool Typed(const TerminalInputEvent& event, View view);
  // Take every drawn prediction back out, restoring the confirmed line and cursor.
  // True when the view changed.
  bool Withdraw(View view);
  // A frame acknowledging input through `echo_ack` was applied to `view`: judge,
  // then re-draw what is still pending. True when the view changed.
  bool Judge(std::uint64_t echo_ack, View view);
  // Drop everything (a cold attach, a closed terminal).
  void Clear();

  std::size_t pending() const { return pending_.size(); }
  bool suppressed() const { return suppressed_; }
  // Whether predictions are drawn at all right now (mode, round trip, suppression).
  bool drawing() const;

 private:
  struct Prediction {
    std::uint64_t seq = 0;
    bool erase = false;
    TerminalCell glyph;
    // Where it was last placed: the column, and the row as a distance from the end
    // of the buffer (scrollback growing above does not move it).
    std::size_t column = 0;
    std::size_t row_from_end = 0;
    bool placed = false;
  };

  // Place the pending predictions from the view's confirmed cursor; draws them
  // when `drawing()` and the view allows it. True when the view changed.
  bool Place(View view);

  Mode mode_ = Mode::Adaptive;
  std::optional<std::chrono::milliseconds> rtt_;
  std::deque<Prediction> pending_;
  bool suppressed_ = false;
  // A non-predictable input was sent at this seq; nothing is predicted until a
  // frame acknowledges it (0 = not blocked).
  std::uint64_t blocked_until_ = 0;
  // What the drawn predictions overwrote: the cursor line as it was, and the
  // confirmed cursor column.
  bool drawn_ = false;
  std::size_t saved_row_ = 0;
  TerminalLine saved_line_;
  std::size_t saved_cursor_column_ = 0;
};

}  // namespace microide::terminal
