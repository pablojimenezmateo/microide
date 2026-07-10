#include "terminal/TerminalSessionInputEncoding.h"

#include "terminal/TerminalMouseEncoder.h"

#include <string>

namespace microide::terminal {

namespace {

using Key = TerminalSession::KeyPress::Key;

int ModifierParam(const TerminalSession::KeyPress& press) {
  return 1 + (press.shift ? 1 : 0) + (press.alt ? 2 : 0) + (press.ctrl ? 4 : 0) +
         (press.super ? 8 : 0);
}

void AppendUtf8(std::string& out, char32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

// CSI-u functional encoding: ESC [ <codepoint> [; <mod>] u
std::string CsiU(char32_t codepoint, int modifier_param) {
  std::string out = "\x1b[";
  out += std::to_string(static_cast<unsigned long>(codepoint));
  if (modifier_param > 1) {
    out += ';';
    out += std::to_string(modifier_param);
  }
  out += 'u';
  return out;
}

// Control byte for a Ctrl+<key> combination (legacy encoding).
char ControlByte(char32_t codepoint) {
  char32_t upper = (codepoint >= 'a' && codepoint <= 'z') ? codepoint - 32 : codepoint;
  if (upper >= 0x40 && upper <= 0x5F) {
    return static_cast<char>(upper & 0x1F);
  }
  if (upper == 0x20) {
    return '\0';  // Ctrl+Space / Ctrl+@
  }
  if (upper == '?') {
    return 0x7F;
  }
  return static_cast<char>(codepoint);
}

}  // namespace

std::string FormatTerminalKeyPress(bool application_cursor_keys_mode,
                                   std::uint8_t kitty_flags,
                                   const TerminalSession::KeyPress& press) {
  const int mod = ModifierParam(press);
  const bool has_mod = mod > 1;
  const bool kitty = kitty_flags != 0;
  const bool report_all = (kitty_flags & 0x08u) != 0;

  // Functional keys that use a trailing letter (CSI <letter> / SS3 <letter>).
  const auto letter_key = [&](char final) -> std::string {
    if (has_mod) {
      return std::string("\x1b[1;") + std::to_string(mod) + final;
    }
    if (application_cursor_keys_mode) {
      return std::string("\x1bO") + final;
    }
    return std::string("\x1b[") + final;
  };
  // Keypad/editing keys that use a numeric prefix and trailing '~'.
  const auto tilde_key = [&](int number) -> std::string {
    std::string out = "\x1b[";
    out += std::to_string(number);
    if (has_mod) {
      out += ';';
      out += std::to_string(mod);
    }
    out += '~';
    return out;
  };
  // F1-F4 use SS3 P/Q/R/S, or CSI 1;mod letter when modified.
  const auto function_letter = [&](char final) -> std::string {
    if (has_mod) {
      return std::string("\x1b[1;") + std::to_string(mod) + final;
    }
    return std::string("\x1bO") + final;
  };

  switch (press.key) {
    case Key::Up:
      return letter_key('A');
    case Key::Down:
      return letter_key('B');
    case Key::Right:
      return letter_key('C');
    case Key::Left:
      return letter_key('D');
    case Key::Home:
      return letter_key('H');
    case Key::End:
      return letter_key('F');
    case Key::Insert:
      return tilde_key(2);
    case Key::Delete:
      return tilde_key(3);
    case Key::PageUp:
      return tilde_key(5);
    case Key::PageDown:
      return tilde_key(6);
    case Key::F1:
      return function_letter('P');
    case Key::F2:
      return function_letter('Q');
    case Key::F3:
      return function_letter('R');
    case Key::F4:
      return function_letter('S');
    case Key::F5:
      return tilde_key(15);
    case Key::F6:
      return tilde_key(17);
    case Key::F7:
      return tilde_key(18);
    case Key::F8:
      return tilde_key(19);
    case Key::F9:
      return tilde_key(20);
    case Key::F10:
      return tilde_key(21);
    case Key::F11:
      return tilde_key(23);
    case Key::F12:
      return tilde_key(24);
    case Key::Enter:
      if (kitty && (has_mod || report_all)) {
        return CsiU(13, mod);
      }
      return press.alt ? std::string("\x1b\r") : std::string("\r");
    case Key::Tab:
      if (kitty && (has_mod || report_all)) {
        return CsiU(9, mod);
      }
      if (press.shift) {
        return "\x1b[Z";  // CBT — backward tab.
      }
      return press.alt ? std::string("\x1b\t") : std::string("\t");
    case Key::Backspace:
      if (kitty && (has_mod || report_all)) {
        return CsiU(127, mod);
      }
      if (press.ctrl) {
        return std::string(1, '\x08');
      }
      return press.alt ? std::string("\x1b\x7f") : std::string("\x7f");
    case Key::Escape:
      if (kitty && (has_mod || report_all)) {
        return CsiU(27, mod);
      }
      return "\x1b";
    case Key::Char: {
      const char32_t cp = press.codepoint;
      if (cp == 0) {
        return {};
      }
      // Under Kitty, modified printable keys (and everything when "report all"
      // is set) are disambiguated as CSI-u over the base codepoint.
      if (kitty && (has_mod || report_all)) {
        char32_t base = (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
        return CsiU(base, mod);
      }
      if (press.ctrl) {
        // Meta+Ctrl: xterm prefixes the control byte with ESC (Alt/Meta) so apps
        // that bind M-C-<key> (Emacs, tmux, readline) see both modifiers. Dropping
        // the ESC here made those chords indistinguishable from plain Ctrl.
        if (press.alt) {
          return std::string("\x1b") + ControlByte(cp);
        }
        return std::string(1, ControlByte(cp));
      }
      if (press.alt) {
        std::string out = "\x1b";
        AppendUtf8(out, cp);
        return out;
      }
      std::string out;
      AppendUtf8(out, cp);
      return out;
    }
  }
  return {};
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
