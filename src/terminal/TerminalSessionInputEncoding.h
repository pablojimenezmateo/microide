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

inline std::string FormatTerminalKeyBytes(bool application_cursor_keys_mode,
                                          TerminalSession::Key key) {
  switch (key) {
    case TerminalSession::Key::Escape:
      return "\x1b";
    case TerminalSession::Key::Enter:
      return "\r";
    case TerminalSession::Key::Backspace:
      return "\x7f";
    case TerminalSession::Key::Tab:
      return "\t";
    case TerminalSession::Key::Up:
      return application_cursor_keys_mode ? "\x1bOA" : "\x1b[A";
    case TerminalSession::Key::Down:
      return application_cursor_keys_mode ? "\x1bOB" : "\x1b[B";
    case TerminalSession::Key::Right:
      return application_cursor_keys_mode ? "\x1bOC" : "\x1b[C";
    case TerminalSession::Key::Left:
      return application_cursor_keys_mode ? "\x1bOD" : "\x1b[D";
    case TerminalSession::Key::Home:
      return application_cursor_keys_mode ? "\x1bOH" : "\x1b[H";
    case TerminalSession::Key::End:
      return application_cursor_keys_mode ? "\x1bOF" : "\x1b[F";
    case TerminalSession::Key::PageUp:
      return "\x1b[5~";
    case TerminalSession::Key::PageDown:
      return "\x1b[6~";
    case TerminalSession::Key::Insert:
      return "\x1b[2~";
    case TerminalSession::Key::Delete:
      return "\x1b[3~";
    default:
      return {};
  }
}

// Encode a modified key press. When `kitty_flags` is non-zero the Kitty
// keyboard protocol is active and keys the legacy encoding cannot disambiguate
// (modified Enter/Tab/Backspace/Escape, control letters) are emitted in CSI-u
// form; otherwise the standard xterm encoding (with CSI `1;mod` / `n;mod~`
// modifier parameters and SS3/CSI application-cursor handling) is used.
std::string FormatTerminalKeyPress(bool application_cursor_keys_mode,
                                   std::uint8_t kitty_flags,
                                   const TerminalSession::KeyPress& press);

inline std::string FormatTerminalPasteBytes(bool bracketed_paste_mode, std::string_view text) {
  if (!bracketed_paste_mode) {
    return std::string(text);
  }

  std::string bytes;
  bytes.reserve(text.size() + 12);
  bytes.append("\x1b[200~");
  bytes.append(text);
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
                                     SDL_Keymod modifiers,
                                     bool can_encode,
                                     std::string& out_bytes);

}  // namespace microide::terminal
