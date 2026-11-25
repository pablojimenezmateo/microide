#include "terminal/TerminalSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace microide::terminal {

void TerminalSession::HandleKittyKeyboardLocked(char prefix,
                                                char ,
                                                const std::vector<int>& params) {
  constexpr std::size_t kMaxKittyStackDepth = 16;
  const auto param = [&](std::size_t index, int fallback) {
    return index < params.size() ? params[index] : fallback;
  };
  switch (prefix) {
    case '?':
      SendBytesLocked("\x1b[?" + std::to_string(static_cast<int>(kitty_keyboard_flags_)) + "u");
      return;
    case '>': {
      if (kitty_keyboard_stack_.size() >= kMaxKittyStackDepth) {
        kitty_keyboard_stack_.erase(kitty_keyboard_stack_.begin());
      }
      kitty_keyboard_stack_.push_back(kitty_keyboard_flags_);
      kitty_keyboard_flags_ = static_cast<std::uint8_t>(param(0, 0) & 0xFF);
      return;
    }
    case '<': {
      const std::size_t requested = static_cast<std::size_t>(std::max(1, param(0, 1)));
      const std::size_t pops = std::min(requested, kitty_keyboard_stack_.size());
      for (std::size_t i = 0; i < pops; ++i) {
        kitty_keyboard_flags_ = kitty_keyboard_stack_.back();
        kitty_keyboard_stack_.pop_back();
      }
      if (pops < requested) {
        kitty_keyboard_flags_ = 0;
      }
      return;
    }
    case '=': {
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
  switch (mode) {
    case 1:
      return application_cursor_keys_mode_ ? 1 : 2;
    case 5:
      return screen_reverse_video_ ? 1 : 2;
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
      return mouse_.normal ? 1 : 2;
    case 1002:
      return mouse_.drag ? 1 : 2;
    case 1003:
      return mouse_.any ? 1 : 2;
    case 1004:
      return focus_event_mode_ ? 1 : 2;
    case 1006:
      return mouse_.sgr ? 1 : 2;
    case 2004:
      return bracketed_paste_mode_ ? 1 : 2;
    case 2026:
      return synchronized_output_ ? 1 : 2;
    case 2031:
      return color_scheme_updates_ ? 1 : 2;
    default:
      return 0;
  }
}

void TerminalSession::HandlePrivateModeLocked(int mode, bool enabled) {
  switch (mode) {
    case 1:
      application_cursor_keys_mode_ = enabled;
      return;
    case 5:
      if (screen_reverse_video_ != enabled) {
        screen_reverse_video_ = enabled;
        AdvanceSnapshotGenerationLocked();
      }
      return;
    case 6:
      origin_mode_ = enabled;
      MoveCursorLocked(PrimaryScreenTopLocked() + (enabled ? ActiveScrollRegionTopLocked() : 0), 0);
      return;
    case 7:
      auto_wrap_mode_ = enabled;
      return;
    case 1000:
      mouse_.normal = enabled;
      pointer_.last_motion.reset();
      return;
    case 1002:
      mouse_.drag = enabled;
      pointer_.last_motion.reset();
      return;
    case 1003:
      mouse_.any = enabled;
      pointer_.last_motion.reset();
      return;
    case 1006:
      mouse_.sgr = enabled;
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
    case 2031:
      color_scheme_updates_ = enabled;
      return;
    case 2026:
      if (!enabled && synchronized_output_) {
        sync_frame_completed_ = true;
      }
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

}
