#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalPointerShape.h"
#include "util/KeyModifiers.h"

namespace microide::terminal {

enum class TerminalMouseButton : std::uint8_t {
  Left,
  Middle,
  Right,
  None,
  WheelUp,
  WheelDown,
};

struct TerminalMouseModes {
  bool normal = false;
  bool drag = false;
  bool any = false;
  bool sgr = false;
  TerminalPointerShape pointer_shape = TerminalPointerShape::None;
};

struct TerminalMotionCell {
  std::size_t row = 0;
  std::size_t column = 0;
  TerminalMouseButton button = TerminalMouseButton::None;
  friend bool operator==(const TerminalMotionCell&, const TerminalMotionCell&) = default;
};

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
  char32_t codepoint = 0;
  bool shift = false;
  bool alt = false;
  bool ctrl = false;
  bool super = false;
};

struct TerminalInputEvent {
  enum class Kind : std::uint8_t {
    Key,
    Paste,
    MouseButton,
    MouseMotion,
    Focus,
    Bytes,
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

}
