#include "terminal/TerminalSessionInputEncoding.h"

#include "terminal/TerminalMouseEncoder.h"

namespace microide::terminal {

bool EncodeTerminalSessionMouseEvent(bool mouse_tracking_any,
                                     bool mouse_tracking_drag,
                                     bool mouse_tracking_normal,
                                     bool mouse_sgr_ext_mode,
                                     std::size_t rows,
                                     std::size_t columns,
                                     TerminalSession::MouseButton button,
                                     bool pressed,
                                     bool motion,
                                     std::size_t row,
                                     std::size_t column,
                                     SDL_Keymod modifiers,
                                     bool can_encode,
                                     std::string& out_bytes) {
  if (!can_encode) {
    out_bytes.clear();
    return false;
  }

  TerminalMouseButton encoded_button = TerminalMouseButton::None;
  switch (button) {
    case TerminalSession::MouseButton::Left:
      encoded_button = TerminalMouseButton::Left;
      break;
    case TerminalSession::MouseButton::Middle:
      encoded_button = TerminalMouseButton::Middle;
      break;
    case TerminalSession::MouseButton::Right:
      encoded_button = TerminalMouseButton::Right;
      break;
    case TerminalSession::MouseButton::WheelUp:
      encoded_button = TerminalMouseButton::WheelUp;
      break;
    case TerminalSession::MouseButton::WheelDown:
      encoded_button = TerminalMouseButton::WheelDown;
      break;
    case TerminalSession::MouseButton::None:
    default:
      encoded_button = TerminalMouseButton::None;
      break;
  }

  return EncodeTerminalMouseEvent(
      TerminalMouseEncodeRequest{
          .tracking_mode = CurrentTerminalMouseTrackingMode(mouse_tracking_any,
                                                              mouse_tracking_drag,
                                                              mouse_tracking_normal),
          .mouse_sgr_ext_mode = mouse_sgr_ext_mode,
          .rows = rows,
          .columns = columns,
          .button = encoded_button,
          .pressed = pressed,
          .motion = motion,
          .row = row,
          .column = column,
          .modifiers = modifiers,
      },
      out_bytes);
}

}  // namespace microide::terminal
