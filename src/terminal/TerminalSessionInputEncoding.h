#pragma once

#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalSession.h"

#include <string>
#include <string_view>

namespace microide::terminal {

inline TerminalMouseTrackingMode CurrentTerminalMouseTrackingMode(bool mouse_tracking_any,
                                                                  bool mouse_tracking_drag,
                                                                  bool mouse_tracking_normal) {
  if (mouse_tracking_any) {
    return TerminalMouseTrackingMode::Any;
  }
  if (mouse_tracking_drag) {
    return TerminalMouseTrackingMode::Drag;
  }
  if (mouse_tracking_normal) {
    return TerminalMouseTrackingMode::Normal;
  }
  return TerminalMouseTrackingMode::Disabled;
}

std::string FormatTerminalKeyPress(bool application_cursor_keys_mode,
                                   std::uint8_t kitty_flags,
                                   const TerminalSession::KeyPress& press);

inline std::string FormatTerminalPasteBytes(bool bracketed_paste_mode, std::string_view text) {
  const auto push_normalized = [](std::string& out, char c, char previous) {
    if (c == '\n') {
      if (previous != '\r') {
        out.push_back('\r');
      }
      return;
    }
    out.push_back(c);
  };
  if (!bracketed_paste_mode) {
    std::string bytes;
    bytes.reserve(text.size());
    char previous = '\0';
    for (const char c : text) {
      push_normalized(bytes, c, previous);
      previous = c;
    }
    return bytes;
  }

  static constexpr std::string_view kEndMarker = "\x1b[201~";
  std::string body;
  body.reserve(text.size());
  char previous = '\0';
  for (const char c : text) {
    push_normalized(body, c, previous);
    previous = c;
    if (body.size() >= kEndMarker.size() &&
        std::string_view(body).substr(body.size() - kEndMarker.size()) == kEndMarker) {
      body.erase(body.size() - kEndMarker.size());
    }
  }
  std::string bytes;
  bytes.reserve(body.size() + 12);
  bytes.append("\x1b[200~");
  bytes.append(body);
  bytes.append("\x1b[201~");
  return bytes;
}

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
                                     util::KeyModifiers modifiers,
                                     bool can_encode,
                                     std::string& out_bytes);

}
