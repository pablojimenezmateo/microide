#include "terminal/TerminalMouseEncoder.h"

#include <algorithm>

namespace microide::terminal {

namespace {

int MouseModifierBits(SDL_Keymod modifiers) {
  int bits = 0;
  if ((modifiers & SDL_KMOD_SHIFT) != 0) {
    bits |= 4;
  }
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    bits |= 8;
  }
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    bits |= 16;
  }
  return bits;
}

}  // namespace

bool EncodeTerminalMouseEvent(const TerminalMouseEncodeRequest& request, std::string& out_bytes) {
  out_bytes.clear();
  if (request.tracking_mode == TerminalMouseTrackingMode::Disabled) {
    return false;
  }
  if (request.motion) {
    if (request.tracking_mode == TerminalMouseTrackingMode::Normal) {
      return false;
    }
    if (request.tracking_mode == TerminalMouseTrackingMode::Drag &&
        request.button == TerminalMouseButton::None) {
      return false;
    }
  }

  const std::size_t clamped_row =
      request.rows > 0 ? std::min(request.row, request.rows - 1) : request.row;
  const std::size_t clamped_column =
      request.columns > 0 ? std::min(request.column, request.columns - 1) : request.column;

  int code = 0;
  switch (request.button) {
    case TerminalMouseButton::Left:
      code = 0;
      break;
    case TerminalMouseButton::Middle:
      code = 1;
      break;
    case TerminalMouseButton::Right:
      code = 2;
      break;
    case TerminalMouseButton::WheelUp:
      code = 64;
      break;
    case TerminalMouseButton::WheelDown:
      code = 65;
      break;
    case TerminalMouseButton::None:
    default:
      code = 3;
      break;
  }

  if (!request.mouse_sgr_ext_mode && !request.pressed && !request.motion &&
      request.button != TerminalMouseButton::WheelUp &&
      request.button != TerminalMouseButton::WheelDown) {
    // Legacy X10/normal encoding cannot distinguish which button was released, so
    // it collapses every release to the ambiguous button-3 code. SGR (1006) mode
    // must NOT: it signals release via the trailing 'm' and keeps the real button
    // number in Pb, so gate this override off when SGR extended reporting is on.
    code = 3;
  }
  if (request.motion) {
    code |= 32;
  }
  code |= MouseModifierBits(request.modifiers);

  if (request.mouse_sgr_ext_mode) {
    out_bytes = "\x1b[<" + std::to_string(code) + ";" + std::to_string(clamped_column + 1) + ";" +
                std::to_string(clamped_row + 1) +
                ((request.pressed || request.motion || request.button == TerminalMouseButton::WheelUp ||
                  request.button == TerminalMouseButton::WheelDown)
                     ? "M"
                     : "m");
    return true;
  }

  // Legacy X10 encoding can only represent coordinates up to column/row 223
  // (cell + 33 must fit in one byte). Beyond that, clamping to the edge cell
  // would deliver the event at the wrong location; drop it instead so the app
  // never sees a phantom edge click. Terminals this large should enable SGR
  // (1006) reporting, which has no such limit.
  if (clamped_column > 222 || clamped_row > 222) {
    return false;
  }
  const int encoded_button = std::clamp(32 + code, 0, 255);
  const int encoded_column = 33 + static_cast<int>(clamped_column);
  const int encoded_row = 33 + static_cast<int>(clamped_row);
  out_bytes.push_back('\x1b');
  out_bytes.push_back('[');
  out_bytes.push_back('M');
  out_bytes.push_back(static_cast<char>(encoded_button));
  out_bytes.push_back(static_cast<char>(encoded_column));
  out_bytes.push_back(static_cast<char>(encoded_row));
  return true;
}

}  // namespace microide::terminal
