#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalSessionEscapeInternal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace microide::terminal {

void TerminalSession::HandleEscapeSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != '[') {
    return;
  }

  const char final = sequence.back();
  std::string_view body = sequence.substr(1, sequence.size() - 2);
  // Trailing intermediate byte (0x20..0x2F), e.g. '$' for DECRQM (`CSI ? Ps $ p`)
  // or ' ' for DECSCUSR (`CSI Ps SP q`). Stripped so the parameter list is clean.
  char intermediate = '\0';
  if (!body.empty()) {
    const unsigned char back = static_cast<unsigned char>(body.back());
    if (back >= 0x20 && back <= 0x2F) {
      intermediate = static_cast<char>(back);
      body.remove_suffix(1);
    }
  }
  char prefix = '\0';
  if (!body.empty() && !std::isdigit(static_cast<unsigned char>(body.front())) &&
      body.front() != ';') {
    prefix = body.front();
    body.remove_prefix(1);
  }
  std::vector<int> params = ParseCsiParameters(body);

  // Kitty keyboard protocol negotiation: CSI ? u (query), CSI > flags u (push),
  // CSI < n u (pop), CSI = flags ; mode u (set).
  if (final == 'u' && (prefix == '?' || prefix == '>' || prefix == '<' || prefix == '=')) {
    HandleKittyKeyboardLocked(prefix, final, params);
    return;
  }

  if (prefix == '?') {
    if (final == 'p' && intermediate == '$') {
      // DECRQM: report each requested private mode's current state.
      for (const int mode : params) {
        SendBytesLocked("\x1b[?" + std::to_string(mode) + ";" +
                        std::to_string(QueryPrivateModeStateLocked(mode)) + "$y");
      }
      return;
    }
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
    case 'm':
      detail::ApplySgrParameters(current_style_, body);
      return;
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
    case 'I': {  // CHT — cursor forward tabulation.
      const int count = CsiParamOrDefault(params, 0, 1);
      for (int i = 0; i < count; ++i) {
        cursor_column_ = NextTabStopLocked(cursor_column_);
      }
      return;
    }
    case 'Z': {  // CBT — cursor backward tabulation.
      const int count = CsiParamOrDefault(params, 0, 1);
      for (int i = 0; i < count; ++i) {
        cursor_column_ = PreviousTabStopLocked(cursor_column_);
      }
      return;
    }
    case 'g': {  // TBC — tab clear.
      if (tab_stops_.empty()) {
        ResetTabStopsLocked();
      }
      if (!params.empty() && params.front() == 3) {
        std::fill(tab_stops_.begin(), tab_stops_.end(), false);
      } else if (cursor_column_ < tab_stops_.size()) {
        tab_stops_[cursor_column_] = false;
      }
      return;
    }
    case 'q':
      if (intermediate == ' ') {  // DECSCUSR — set cursor style.
        switch (CsiParamOrDefault(params, 0, 1)) {
          case 2:
            cursor_shape_ = CursorShape::Block;
            cursor_blinking_ = false;
            break;
          case 3:
            cursor_shape_ = CursorShape::Underline;
            cursor_blinking_ = true;
            break;
          case 4:
            cursor_shape_ = CursorShape::Underline;
            cursor_blinking_ = false;
            break;
          case 5:
            cursor_shape_ = CursorShape::Bar;
            cursor_blinking_ = true;
            break;
          case 6:
            cursor_shape_ = CursorShape::Bar;
            cursor_blinking_ = false;
            break;
          default:
            cursor_shape_ = CursorShape::Block;
            cursor_blinking_ = true;
            break;
        }
      }
      return;
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

}  // namespace microide::terminal
