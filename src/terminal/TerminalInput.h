#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalPointerShape.h"
#include "util/KeyModifiers.h"

namespace microide::terminal {

// The terminal's input vocabulary, apart from the session that encodes it: a host
// terminal (dev-docs/design/remote-projects.md § 6.5) ships these SEMANTIC events
// over the wire and encodes them on the host, against the host's own mode state,
// so they cannot be private to TerminalSession.

enum class TerminalMouseButton : std::uint8_t {
  Left,
  Middle,
  Right,
  None,
  WheelUp,
  WheelDown,
};

// The mouse reporting a program asked for: DECSET 1000 (presses), 1002 (drags),
// 1003 (every move) and 1006 (SGR coordinates).
struct TerminalMouseModes {
  bool normal = false;
  bool drag = false;
  bool any = false;
  bool sgr = false;
  // OSC 22's pointer (TerminalPointerShape.h): mouse state a mode reset clears.
  TerminalPointerShape pointer_shape = TerminalPointerShape::None;
};

// The cell (and held button) the last motion report named. A move inside one cell
// is not reported, as in xterm: under any-motion tracking every pixel of pointer
// travel was a report -- bytes into the PTY, a redraw for the program, and a
// message over the wire for a host terminal.
struct TerminalMotionCell {
  std::size_t row = 0;
  std::size_t column = 0;
  TerminalMouseButton button = TerminalMouseButton::None;
  friend bool operator==(const TerminalMotionCell&, const TerminalMotionCell&) = default;
};

// A physical key press carrying its logical key plus active modifiers. The
// session encodes it per the negotiated keyboard protocol (legacy xterm or
// the Kitty keyboard protocol when an application has enabled it).
struct TerminalKeyPress {
  enum class Key : std::uint8_t {
    Char,
    Enter,
    Escape,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
  };
  static constexpr std::uint8_t kLastKey = static_cast<std::uint8_t>(Key::F12);

  Key key = Key::Char;
  char32_t codepoint = 0;  // Base-layout codepoint for Key::Char.
  bool shift = false;
  bool alt = false;
  bool ctrl = false;
  bool super = false;
};

// One input event as a host terminal receives it. `seq` is the client's
// monotonically increasing input sequence number: the host echoes the latest one
// it has written to the pty back as `echo_ack`, which is what the client's echo
// prediction validates against.
struct TerminalInputEvent {
  enum class Kind : std::uint8_t {
    Key,          // key
    Paste,        // text, bracketed on the host if the program asked for it
    MouseButton,  // button, pressed, row, column, modifiers
    MouseMotion,  // button, row, column, modifiers
    Focus,        // pressed = focused
    Bytes,        // text, written verbatim (a scripted command, a relaunch)
  };
  static constexpr std::uint8_t kLastKind = static_cast<std::uint8_t>(Kind::Bytes);

  Kind kind = Kind::Key;
  std::uint64_t seq = 0;
  TerminalKeyPress key;
  std::string text;
  TerminalMouseButton button = TerminalMouseButton::None;
  bool pressed = false;
  std::uint32_t row = 0;
  std::uint32_t column = 0;
  util::KeyModifiers modifiers = util::kKeyModNone;
};

}  // namespace microide::terminal
