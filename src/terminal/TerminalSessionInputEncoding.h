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

  // Neutralize any embedded end marker in the pasted text: otherwise a poisoned
  // clipboard containing `ESC[201~<payload>` would terminate paste mode early and
  // deliver <payload> to the shell as if typed — the exact injection bracketed
  // paste exists to prevent. Drop the marker (as xterm does) and keep the rest.
  //
  // A single non-overlapping deletion pass is NOT enough: deleting one marker can
  // splice its surrounding bytes into a *new* marker at the seam (e.g. the paste
  // `ESC[` + `ESC[201~` + `201~payload` collapses to `ESC[201~payload`). We instead
  // build the body incrementally and, whenever its tail forms the marker, erase it —
  // a fixed point that also catches deletion-created markers. The body is kept
  // separate from the wrapping markers so the intentional guards aren't rescanned.
  static constexpr std::string_view kEndMarker = "\x1b[201~";
  std::string body;
  body.reserve(text.size());
  for (const char c : text) {
    body.push_back(c);
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
                                     SDL_Keymod modifiers,
                                     bool can_encode,
                                     std::string& out_bytes);

}  // namespace microide::terminal
