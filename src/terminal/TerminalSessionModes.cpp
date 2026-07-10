#include "terminal/TerminalSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace microide::terminal {

void TerminalSession::HandleKittyKeyboardLocked(char prefix,
                                                char /*final*/,
                                                const std::vector<int>& params) {
  constexpr std::size_t kMaxKittyStackDepth = 16;
  const auto param = [&](std::size_t index, int fallback) {
    return index < params.size() ? params[index] : fallback;
  };
  switch (prefix) {
    case '?':  // Query current flags.
      SendBytesLocked("\x1b[?" + std::to_string(static_cast<int>(kitty_keyboard_flags_)) + "u");
      return;
    case '>': {  // Push flags.
      if (kitty_keyboard_stack_.size() >= kMaxKittyStackDepth) {
        kitty_keyboard_stack_.erase(kitty_keyboard_stack_.begin());
      }
      kitty_keyboard_stack_.push_back(kitty_keyboard_flags_);
      kitty_keyboard_flags_ = static_cast<std::uint8_t>(param(0, 0) & 0xFF);
      return;
    }
    case '<': {  // Pop flags.
      int count = std::max(1, param(0, 1));
      while (count-- > 0) {
        if (kitty_keyboard_stack_.empty()) {
          kitty_keyboard_flags_ = 0;
        } else {
          kitty_keyboard_flags_ = kitty_keyboard_stack_.back();
          kitty_keyboard_stack_.pop_back();
        }
      }
      return;
    }
    case '=': {  // Set flags with mode (1=set, 2=union, 3=clear bits).
      const auto flags = static_cast<std::uint8_t>(param(0, 0) & 0xFF);
      switch (param(1, 1)) {
        case 2:
          kitty_keyboard_flags_ |= flags;
          break;
        case 3:
          kitty_keyboard_flags_ &= static_cast<std::uint8_t>(~flags);
          break;
        case 1:
        default:
          kitty_keyboard_flags_ = flags;
          break;
      }
      return;
    }
    default:
      return;
  }
}

int TerminalSession::QueryPrivateModeStateLocked(int mode) const {
  // DECRQM reply value: 1 = set, 2 = reset, 0 = not recognized.
  switch (mode) {
    case 1:
      return application_cursor_keys_mode_ ? 1 : 2;
    case 6:
      return origin_mode_ ? 1 : 2;
    case 7:
      return auto_wrap_mode_ ? 1 : 2;
    case 12:
      return cursor_blinking_ ? 1 : 2;
    case 25:
      return cursor_visible_ ? 1 : 2;
    case 47:
    case 1047:
    case 1049:
      return use_alternate_screen_ ? 1 : 2;
    case 1000:
      return mouse_tracking_normal_ ? 1 : 2;
    case 1002:
      return mouse_tracking_drag_ ? 1 : 2;
    case 1003:
      return mouse_tracking_any_ ? 1 : 2;
    case 1004:
      return focus_event_mode_ ? 1 : 2;
    case 1006:
      return mouse_sgr_ext_mode_ ? 1 : 2;
    case 2004:
      return bracketed_paste_mode_ ? 1 : 2;
    case 2026:
      return synchronized_output_ ? 1 : 2;
    default:
      return 0;
  }
}

void TerminalSession::HandlePrivateModeLocked(int mode, bool enabled) {
  switch (mode) {
    case 1:
      application_cursor_keys_mode_ = enabled;
      return;
    case 6:
      origin_mode_ = enabled;
      // DECOM set/reset homes the cursor. On the primary buffer cursor_row_ is an
      // absolute scrollback index, so home to the visible-screen top — mirroring CUP
      // (`CSI H`), which ignores origin/scroll-region on primary. Passing a
      // screen-relative 0 here (as before) jumped into scrollback and overwrote
      // history. On the alternate screen, origin mode selects the scroll-region top.
      if (use_alternate_screen_) {
        MoveCursorLocked(enabled ? ActiveScrollRegionTopLocked() : 0, 0);
      } else {
        MoveCursorLocked(PrimaryScreenTopLocked(), 0);
      }
      return;
    case 7:
      auto_wrap_mode_ = enabled;
      return;
    case 1000:
      mouse_tracking_normal_ = enabled;
      return;
    case 1002:
      mouse_tracking_drag_ = enabled;
      return;
    case 1003:
      mouse_tracking_any_ = enabled;
      return;
    case 1006:
      mouse_sgr_ext_mode_ = enabled;
      return;
    case 2004:
      bracketed_paste_mode_ = enabled;
      return;
    case 1004:
      focus_event_mode_ = enabled;
      return;
    case 12:
      cursor_blinking_ = enabled;
      return;
    case 25:
      cursor_visible_ = enabled;
      return;
    case 2026:
      synchronized_output_ = enabled;
      if (!enabled) {
        sync_suppressed_wakes_ = 0;
      }
      return;
    case 47:
    case 1047:
      SetAlternateScreenLocked(enabled, false);
      return;
    case 1048:
      if (enabled) {
        SaveCursorLocked();
      } else {
        RestoreCursorLocked();
      }
      return;
    case 1049:
      if (enabled) {
        SaveCursorLocked();
      }
      SetAlternateScreenLocked(enabled, true);
      if (!enabled) {
        RestoreCursorLocked();
      }
      return;
    default:
      return;
  }
}

}  // namespace microide::terminal
