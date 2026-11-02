#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace microide::terminal {

// The pointer a program named with OSC 22 (kitty, foot, Ghostty and xterm take
// it: `ESC ] 22 ; pointer ST`), or None when it named none and the panel decides
// (TerminalSession::PointerShapeAt). The shapes a desktop cursor theme has; it
// fits in five bits, which is how it rides a host frame's flags
// (TerminalHostFrame::kPointerShapeShift).
enum class TerminalPointerShape : std::uint8_t {
  None,
  Default,
  Text,
  Pointer,
  Wait,
  Progress,
  Crosshair,
  Move,
  NotAllowed,
  EwResize,
  NsResize,
  NwseResize,
  NeswResize,
  NResize,
  EResize,
  SResize,
  WResize,
  NeResize,
  SeResize,
  SwResize,
  NwResize,
};
inline constexpr std::uint8_t kTerminalPointerShapeCount = 21;

// A CSS cursor keyword (what kitty and foot take) or an X cursor-font name (what
// xterm takes: "hand2", "xterm", "left_ptr", ...) as a shape, the nearest one a
// cursor theme has for the few without their own; nullopt for a name not known.
std::optional<TerminalPointerShape> TerminalPointerShapeFromName(std::string_view name);

// OSC 22's payload: kitty's stack forms (">name" pushes, "<" pops) as a set and a
// reset, comma-separated fallbacks (the first known one wins), "?" (a query) as no
// change. nullopt: leave the shape as it is.
std::optional<TerminalPointerShape> TerminalPointerShapeFromOsc22(std::string_view payload);

}  // namespace microide::terminal
