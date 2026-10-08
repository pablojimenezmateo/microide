#pragma once

#include <cstdint>
#include <string>

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

enum class TerminalCursorShape : std::uint8_t {
  Block,
  Underline,
  Bar,
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
