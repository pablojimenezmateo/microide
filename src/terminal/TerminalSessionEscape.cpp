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

namespace {

Uint8 ClampColorComponent(int value) {
  return static_cast<Uint8>(std::clamp(value, 0, 255));
}

// Decode an extended SGR color (codes 38/48/58) from the parameter groups,
// supporting both the legacy semicolon form (`38;5;n`, `38;2;r;g;b`) and the
// ITU T.416 colon form (`38:5:n`, `38:2:r:g:b`, `38:2::r:g:b`). `gi` is advanced
// past any trailing groups consumed by the legacy form.
std::optional<SDL_Color> ParseExtendedSgrColor(const std::vector<std::vector<int>>& groups,
                                               std::size_t& gi) {
  const std::vector<int>& leading = groups[gi];
  std::vector<int> seq;
  if (leading.size() > 1) {
    seq.assign(leading.begin() + 1, leading.end());
  } else {
    if (gi + 1 >= groups.size()) {
      return std::nullopt;
    }
    const int space = groups[gi + 1].empty() ? 0 : groups[gi + 1].front();
    const std::size_t need = space == 2 ? 3 : space == 5 ? 1 : 0;
    ++gi;
    seq.push_back(space);
    for (std::size_t k = 0; k < need && gi + 1 < groups.size(); ++k) {
      ++gi;
      seq.push_back(groups[gi].empty() ? 0 : groups[gi].front());
    }
  }

  if (seq.empty()) {
    return std::nullopt;
  }
  const int space = seq.front();
  if (space == 5 && seq.size() >= 2) {
    return Ansi256Color(seq[1]);
  }
  if (space == 2 && seq.size() >= 4) {
    const std::size_t n = seq.size();
    return MakeTerminalRgbColor(ClampColorComponent(seq[n - 3]), ClampColorComponent(seq[n - 2]),
                                ClampColorComponent(seq[n - 1]));
  }
  return std::nullopt;
}

// xterm `rgb:RRRR/GGGG/BBBB` color reply (8-bit components widened to 16-bit).
std::string FormatOscRgbReply(SDL_Color color) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto component = [](Uint8 value) {
    std::string out;
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0xF]);
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0xF]);
    return out;
  };
  return "rgb:" + component(color.r) + "/" + component(color.g) + "/" + component(color.b);
}

// Extract the filesystem path from an OSC 7 `file://host/path` payload, decoding
// percent-escapes.
std::string DecodeOsc7Path(std::string_view payload) {
  std::string_view path = payload;
  if (path.rfind("file://", 0) == 0) {
    path.remove_prefix(7);
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos) {
      return {};
    }
    path = path.substr(slash);
  }
  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(path.size());
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      const int hi = hex_value(path[i + 1]);
      const int lo = hex_value(path[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(path[i]);
  }
  return out;
}

void ApplySgrParameters(TerminalStyle& style, std::string_view body) {
  const std::vector<std::vector<int>> groups = ParseSgrParameters(body);
  for (std::size_t gi = 0; gi < groups.size(); ++gi) {
    const std::vector<int>& group = groups[gi];
    const int code = group.empty() ? 0 : group.front();
    switch (code) {
      case 0:
        style = TerminalStyle{};
        break;
      case 1:
        style.set(cell_attr::kBold, true);
        break;
      case 2:
        style.set(cell_attr::kDim, true);
        break;
      case 3:
        style.set(cell_attr::kItalic, true);
        break;
      case 4:
        if (group.size() > 1 && group[1] == 0) {
          style.set(cell_attr::kUnderline, false);
          style.set(cell_attr::kDoubleUnderline, false);
        } else if (group.size() > 1 && group[1] == 2) {
          style.set(cell_attr::kDoubleUnderline, true);
          style.set(cell_attr::kUnderline, false);
        } else {
          style.set(cell_attr::kUnderline, true);
          style.set(cell_attr::kDoubleUnderline, false);
        }
        break;
      case 5:
      case 6:
        style.set(cell_attr::kBlink, true);
        break;
      case 7:
        style.set(cell_attr::kInverse, true);
        break;
      case 8:
        style.set(cell_attr::kHidden, true);
        break;
      case 9:
        style.set(cell_attr::kStrikethrough, true);
        break;
      case 21:
        style.set(cell_attr::kDoubleUnderline, true);
        style.set(cell_attr::kUnderline, false);
        break;
      case 22:
        style.set(cell_attr::kBold, false);
        style.set(cell_attr::kDim, false);
        break;
      case 23:
        style.set(cell_attr::kItalic, false);
        break;
      case 24:
        style.set(cell_attr::kUnderline, false);
        style.set(cell_attr::kDoubleUnderline, false);
        break;
      case 25:
        style.set(cell_attr::kBlink, false);
        break;
      case 27:
        style.set(cell_attr::kInverse, false);
        break;
      case 28:
        style.set(cell_attr::kHidden, false);
        break;
      case 29:
        style.set(cell_attr::kStrikethrough, false);
        break;
      case 38:
        if (auto color = ParseExtendedSgrColor(groups, gi)) {
          style.foreground = *color;
        }
        break;
      case 39:
        style.foreground.reset();
        break;
      case 48:
        if (auto color = ParseExtendedSgrColor(groups, gi)) {
          style.background = *color;
        }
        break;
      case 49:
        style.background.reset();
        break;
      case 58:
        // Underline color: parse to consume parameters, then discard (the
        // renderer draws underlines in the foreground color).
        (void)ParseExtendedSgrColor(groups, gi);
        break;
      default:
        if (code >= 30 && code <= 37) {
          style.foreground = BasicAnsiColor(code - 30, style.bold());
        } else if (code >= 40 && code <= 47) {
          style.background = BasicAnsiColor(code - 40, false);
        } else if (code >= 90 && code <= 97) {
          style.foreground = BasicAnsiColor(code - 90, true);
        } else if (code >= 100 && code <= 107) {
          style.background = BasicAnsiColor(code - 100, true);
        }
        // Codes 53/55 (overline) and 59 (default underline color) are accepted
        // and ignored; they do not affect any tracked attribute.
        break;
    }
  }
}

}  // namespace

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
      ApplySgrParameters(current_style_, body);
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
  const std::string_view payload = body.substr(separator + 1);

  if (command == "0" || command == "1" || command == "2") {
    const std::string title = SanitizeOscTitle(payload);
    launch_label_ = title.empty() ? default_launch_label_ : title;
    return;
  }

  if (command == "7") {
    // Working-directory report: OSC 7 ; file://host/path
    std::string decoded = DecodeOsc7Path(payload);
    if (!decoded.empty()) {
      reported_working_directory_ = std::filesystem::path(std::move(decoded));
    }
    return;
  }

  // Default foreground / background / cursor color queries. Applications use
  // these (especially OSC 11) to detect light vs dark backgrounds; answering
  // avoids a startup timeout. Colors mirror the built-in dark palette.
  if (command == "10" || command == "11" || command == "12") {
    if (payload.find('?') != std::string_view::npos) {
      const SDL_Color foreground = BasicAnsiColor(7, true);
      const SDL_Color background = BasicAnsiColor(0, false);
      const SDL_Color color = command == "11" ? background : foreground;
      SendBytesLocked("\x1b]" + std::string(command) + ";" + FormatOscRgbReply(color) + "\x1b\\");
    }
    return;
  }

  if (command == "4") {
    // Palette query: OSC 4 ; index ; ?  -> reply with the indexed color.
    const std::size_t inner = payload.find(';');
    if (inner != std::string_view::npos &&
        payload.find('?', inner) != std::string_view::npos) {
      const int index = std::atoi(std::string(payload.substr(0, inner)).c_str());
      SendBytesLocked("\x1b]4;" + std::to_string(index) + ";" +
                      FormatOscRgbReply(Ansi256Color(index)) + "\x1b\\");
    }
    return;
  }

  // OSC 8 (hyperlinks), 9 (notifications), 133 (shell-integration prompt marks),
  // and palette resets (104/110/111/112) are accepted and intentionally ignored
  // so they never corrupt the screen.
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
