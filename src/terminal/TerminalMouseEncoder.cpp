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

  if (!request.pressed && !request.motion && request.button != TerminalMouseButton::WheelUp &&
      request.button != TerminalMouseButton::WheelDown) {
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

  const int encoded_button = std::clamp(32 + code, 0, 255);
  const int encoded_column =
      std::clamp(33 + static_cast<int>(std::min<std::size_t>(clamped_column, 222)), 0, 255);
  const int encoded_row =
      std::clamp(33 + static_cast<int>(std::min<std::size_t>(clamped_row, 222)), 0, 255);
  out_bytes.push_back('\x1b');
  out_bytes.push_back('[');
  out_bytes.push_back('M');
  out_bytes.push_back(static_cast<char>(encoded_button));
  out_bytes.push_back(static_cast<char>(encoded_column));
  out_bytes.push_back(static_cast<char>(encoded_row));
  return true;
}

}  // namespace microide::terminal
