#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalOscClipboard.h"
#include "terminal/TerminalProcessControl.h"
#include "platform/TerminalBackend.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::terminal {

void TerminalSession::HandleEscapeSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != '[') {
    return;
  }

  const char final = sequence.back();
  std::string_view body = sequence.substr(1, sequence.size() - 2);
  char prefix = '\0';
  if (!body.empty() && !std::isdigit(static_cast<unsigned char>(body.front())) &&
      body.front() != ';') {
    prefix = body.front();
    body.remove_prefix(1);
  }
  std::vector<int> params = ParseCsiParameters(body);

  if (prefix == '?') {
    if (final == 'h' || final == 'l') {
      const bool enabled = final == 'h';
      for (const int mode : params) {
        HandlePrivateModeLocked(mode, enabled);
      }
    } else if (final == 'n') {
      for (const int mode : params) {
        if (mode == 6) {
          SendBytesLocked("\x1b[?" + std::to_string(cursor_row_ + 1) + ";" +
                          std::to_string(cursor_column_ + 1) + "R");
        }
      }
    }
    return;
  }

  if (prefix == '>') {
    if (final == 'c') {
      SendBytesLocked("\x1b[>0;10;1c");
    }
    return;
  }

  switch (final) {
    case 'c':
      SendBytesLocked("\x1b[?1;2c");
      return;
    case 'm': {
      if (params.empty()) {
        params.push_back(0);
      }
      for (std::size_t i = 0; i < params.size(); ++i) {
        const int code = params[i];
        if (code == 0) {
          current_style_ = TerminalStyle{};
          continue;
        }
        if (code == 1) {
          current_style_.bold = true;
          continue;
        }
        if (code == 22) {
          current_style_.bold = false;
          continue;
        }
        if (code == 7) {
          current_style_.inverse = true;
          continue;
        }
        if (code == 27) {
          current_style_.inverse = false;
          continue;
        }
        if (code == 39) {
          current_style_.foreground.reset();
          continue;
        }
        if (code == 49) {
          current_style_.background.reset();
          continue;
        }
        if (code >= 30 && code <= 37) {
          current_style_.foreground = BasicAnsiColor(code - 30, current_style_.bold);
          continue;
        }
        if (code >= 40 && code <= 47) {
          current_style_.background = BasicAnsiColor(code - 40, false);
          continue;
        }
        if (code >= 90 && code <= 97) {
          current_style_.foreground = BasicAnsiColor(code - 90, true);
          continue;
        }
        if (code >= 100 && code <= 107) {
          current_style_.background = BasicAnsiColor(code - 100, true);
          continue;
        }
        if ((code == 38 || code == 48) && i + 1 < params.size()) {
          SDL_Color color{};
          bool parsed = false;
          if (params[i + 1] == 5 && i + 2 < params.size()) {
            color = Ansi256Color(params[i + 2]);
            i += 2;
            parsed = true;
          } else if (params[i + 1] == 2 && i + 4 < params.size()) {
            color = MakeTerminalRgbColor(static_cast<Uint8>(std::clamp(params[i + 2], 0, 255)),
                              static_cast<Uint8>(std::clamp(params[i + 3], 0, 255)),
                              static_cast<Uint8>(std::clamp(params[i + 4], 0, 255)));
            i += 4;
            parsed = true;
          }
          if (!parsed) {
            continue;
          }
          if (code == 38) {
            current_style_.foreground = color;
          } else {
            current_style_.background = color;
          }
        }
      }
      return;
    }
    case 'A':
      cursor_row_ = cursor_row_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                        ? cursor_row_ - static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                        : 0;
      return;
    case 'B':
      MoveCursorLocked(cursor_row_ + static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                       cursor_column_);
      return;
    case 'C': {
      const std::size_t delta = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      const std::size_t next_column = cursor_column_ + delta;
      cursor_column_ =
          columns_ > 0 ? std::min(next_column, columns_ - 1) : next_column;
      return;
    }
    case 'D':
      cursor_column_ = cursor_column_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           ? cursor_column_ -
                                 static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           : 0;
      return;
    case 'E':
      MoveCursorLocked(cursor_row_ + static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                       0);
      return;
    case 'F':
      MoveCursorLocked(cursor_row_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           ? cursor_row_ -
                                 static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           : 0,
                       0);
      return;
    case 'G':
      MoveCursorLocked(cursor_row_,
                       static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)));
      return;
    case 'H':
    case 'f':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              ((use_alternate_screen_ && origin_mode_) ? ActiveScrollRegionTopLocked() : 0),
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 1, 1) - 1)));
      return;
    case 'J':
      EraseInDisplayLocked(params.empty() ? 0 : params.front());
      return;
    case 'K':
      EraseInLineLocked(params.empty() ? 0 : params.front());
      return;
    case 'L': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      if (use_alternate_screen_) {
        const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
        const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
        if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
          ScrollRegionDownLocked(cursor_row_, scroll_region_bottom, count);
          return;
        }
      }
      lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_), count,
                    TerminalLine{});
      TrimScrollbackLocked();
      return;
    }
    case 'M': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      if (use_alternate_screen_) {
        const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
        const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
        if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
          ScrollRegionUpLocked(cursor_row_, scroll_region_bottom, count);
          return;
        }
      }
      const std::size_t erase_end = std::min(lines_.size(), cursor_row_ + count);
      lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_),
                   lines_.begin() + static_cast<std::ptrdiff_t>(erase_end));
      EnsureCursorLineExistsLocked();
      return;
    }
    case 'P': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      if (cursor_column_ < line.cells.size()) {
        const std::size_t erase_end = std::min(line.cells.size(), cursor_column_ + count);
        line.cells.erase(line.cells.begin() + static_cast<std::ptrdiff_t>(cursor_column_),
                         line.cells.begin() + static_cast<std::ptrdiff_t>(erase_end));
      }
      return;
    }
    case 'X': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      ClearLineRangeLocked(lines_[cursor_row_], cursor_column_, cursor_column_ + count);
      return;
    }
    case '@': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      ResizeLineLocked(line, cursor_column_);
      line.cells.insert(line.cells.begin() + static_cast<std::ptrdiff_t>(cursor_column_), count,
                        MakeAsciiTerminalCell(' ', current_style_));
      if (columns_ > 0 && line.cells.size() > columns_) {
        line.cells.resize(columns_);
      }
      return;
    }
    case 'S':
      if (use_alternate_screen_) {
        ScrollRegionUpLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                             static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'T':
      if (use_alternate_screen_) {
        ScrollRegionDownLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                               static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'n':
      if (!params.empty()) {
        if (params.front() == 5) {
          SendBytesLocked("\x1b[0n");
        } else if (params.front() == 6) {
          SendBytesLocked("\x1b[" + std::to_string(cursor_row_ + 1) + ";" +
                          std::to_string(cursor_column_ + 1) + "R");
        }
      }
      return;
    case 'd':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              ((use_alternate_screen_ && origin_mode_) ? ActiveScrollRegionTopLocked() : 0),
          cursor_column_);
      return;
    case 'r': {
      const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
      const int terminal_rows_int = static_cast<int>(terminal_rows);
      const int scroll_region_top_param = CsiParamOrDefault(params, 0, 1);
      const int scroll_region_bottom_param =
          params.size() > 1 ? CsiParamOrDefault(params, 1, terminal_rows_int) : terminal_rows_int;
      const std::size_t scroll_region_top =
          static_cast<std::size_t>(std::max(0, scroll_region_top_param - 1));
      const std::size_t scroll_region_bottom =
          static_cast<std::size_t>(std::max(0, scroll_region_bottom_param - 1));
      if (scroll_region_top >= terminal_rows || scroll_region_bottom >= terminal_rows ||
          scroll_region_top > scroll_region_bottom) {
        return;
      }
      scroll_region_top_ = scroll_region_top;
      scroll_region_bottom_ = scroll_region_bottom;
      MoveCursorLocked(0, 0);
      return;
    }
    case 's':
      saved_cursor_row_ = cursor_row_;
      saved_cursor_column_ = cursor_column_;
      return;
    case 'u':
      MoveCursorLocked(saved_cursor_row_, saved_cursor_column_);
      return;
    default:
      return;
  }
}

void TerminalSession::HandleOscSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != ']') {
    return;
  }

  if (const auto clipboard = DecodeOsc52ClipboardPayload(sequence)) {
    pending_clipboard_text_ = *clipboard;
    return;
  }

  const std::string_view body = sequence.substr(1);
  const std::size_t separator = body.find(';');
  if (separator == std::string_view::npos) {
    return;
  }

  const std::string_view command = body.substr(0, separator);
  if (command != "0" && command != "1" && command != "2") {
    return;
  }

  const std::string title = SanitizeOscTitle(body.substr(separator + 1));
  launch_label_ = title.empty() ? default_launch_label_ : title;
}

void TerminalSession::HandlePrivateModeLocked(int mode, bool enabled) {
  switch (mode) {
    case 1:
      application_cursor_keys_mode_ = enabled;
      return;
    case 6:
      origin_mode_ = enabled;
      if (enabled) {
        MoveCursorLocked(ActiveScrollRegionTopLocked(), 0);
      } else {
        MoveCursorLocked(0, 0);
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
    case 25:
      cursor_visible_ = enabled;
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
