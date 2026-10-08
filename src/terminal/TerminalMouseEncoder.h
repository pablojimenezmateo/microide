#pragma once

#include <cstddef>
#include <string>

#include "terminal/TerminalInput.h"
#include "util/KeyModifiers.h"

namespace microide::terminal {

enum class TerminalMouseTrackingMode {
  Disabled,
  Normal,
  Drag,
  Any,
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
  util::KeyModifiers modifiers = util::kKeyModNone;
};

bool EncodeTerminalMouseEvent(const TerminalMouseEncodeRequest& request, std::string& out_bytes);

}  // namespace microide::terminal
