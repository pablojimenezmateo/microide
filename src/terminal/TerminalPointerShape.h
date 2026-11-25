#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace microide::terminal {

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

std::optional<TerminalPointerShape> TerminalPointerShapeFromName(std::string_view name);

std::optional<TerminalPointerShape> TerminalPointerShapeFromOsc22(std::string_view payload);

}
