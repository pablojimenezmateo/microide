#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <string>

namespace microide::terminal {

enum class TerminalMouseTrackingMode {
  Disabled,
  Normal,
  Drag,
  Any,
};

enum class TerminalMouseButton {
  Left,
  Middle,
  Right,
  None,
  WheelUp,
  WheelDown,
};

struct TerminalMouseEncodeRequest {
  TerminalMouseTrackingMode tracking_mode = TerminalMouseTrackingMode::Disabled;
  bool mouse_sgr_ext_mode = false;
  std::size_t rows = 0;
  std::size_t columns = 0;
  TerminalMouseButton button = TerminalMouseButton::None;
  bool pressed = false;
  bool motion = false;
  std::size_t row = 0;
  std::size_t column = 0;
  SDL_Keymod modifiers = SDL_KMOD_NONE;
};

bool EncodeTerminalMouseEvent(const TerminalMouseEncodeRequest& request, std::string& out_bytes);

}  // namespace microide::terminal
