#include "terminal/TerminalSession.h"

#include "terminal/TerminalInternalConstants.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalSessionEscapeInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "util/StringUtil.h"

#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif

namespace microide::terminal {

namespace {

std::string CursorPositionReport(bool private_form, std::size_t cursor_row,
                                 std::size_t cursor_column, std::size_t origin_row,
                                 std::size_t columns) {
  const std::size_t reported_column = columns > 0 ? std::min(cursor_column, columns - 1)
                                                  : cursor_column;
  const std::size_t reported_row = cursor_row > origin_row ? cursor_row - origin_row : 0;
  return (private_form ? "\x1b[?" : "\x1b[") + std::to_string(reported_row + 1) + ";" +
         std::to_string(reported_column + 1) + "R";
}

struct ScrollSpan {
  std::size_t top = 0;
  std::size_t bottom = 0;
};

std::optional<ScrollSpan> CursorScrollSpan(std::size_t cursor_row, std::size_t screen_top,
                                           std::size_t region_top, std::size_t region_bottom) {
  if (cursor_row < screen_top + region_top || cursor_row > screen_top + region_bottom) {
    return std::nullopt;
  }
  return ScrollSpan{.top = cursor_row - screen_top, .bottom = region_bottom};
}

}

void TerminalSession::HandleEscapeSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != '[') {
    return;
  }

  const char final = sequence.back();
  std::string_view body = sequence.substr(1, sequence.size() - 2);
  char intermediate = '\0';
  if (!body.empty()) {
    const unsigned char back = static_cast<unsigned char>(body.back());
    if (back >= 0x20 && back <= 0x2F) {
      intermediate = static_cast<char>(back);
      body.remove_suffix(1);
    }
  }
  char prefix = '\0';
  if (!body.empty() && !util::IsAsciiDigit(static_cast<unsigned char>(body.front())) &&
      body.front() != ';') {
    prefix = body.front();
    body.remove_prefix(1);
  }
  if (prefix == '\0' && intermediate == '\0' && final == 'm') {
    if (detail::ApplySgrParameters(current_style_, pen_underline_color_, body)) {
      current_extra_ = InternExtraLocked(pen_link_uri_, pen_underline_color_);
    }
    return;
  }
  thread_local std::vector<int> params_scratch;
  ParseCsiParametersInto(body, params_scratch);
  std::vector<int>& params = params_scratch;


  if (final == 'u' && (prefix == '?' || prefix == '>' || prefix == '<' || prefix == '=')) {
    HandleKittyKeyboardLocked(prefix, final, params);
    return;
  }

  if (prefix == '?') {
    HandlePrivateCsiLocked(final, intermediate, params);
    return;
  }

  if (prefix == '>') {
    if (final == 'c') {
      SendBytesLocked("\x1b[>0;10;1c");
    } else if (final == 'q' && intermediate == '\0' && (params.empty() || params.front() == 0)) {
      SendBytesLocked("\x1bP>|microide " MICROIDE_VERSION "\x1b\\");
    }
    return;
  }

  switch (final) {
    case 'c':
    case 'm':
    case 'q':
    case 'g':
    case 'r':
    case 'h':
    case 'l':
      break;
    default:
      ClampPendingWrapColumn(cursor_column_, columns_);
      break;
  }
  if (final == 'p' && intermediate == '!') {
    SoftResetLocked();
    return;
  }
  const auto count_param = [&params]() {
    return static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
  };
  const auto position_param = [&params](std::size_t index) {
    return static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, index, 1) - 1));
  };
  switch (final) {
    case 'c':
      SendBytesLocked("\x1b[?1;2c");
      return;
    case 'm':
      if (detail::ApplySgrParameters(current_style_, pen_underline_color_, body)) {
        current_extra_ = InternExtraLocked(pen_link_uri_, pen_underline_color_);
      }
      return;
    case 'A':
      cursor_row_ = CursorRowUpLocked(count_param());
      return;
    case 'B':
      MoveCursorLocked(cursor_row_ + count_param(), cursor_column_);
      return;
    case 'C': {
      const std::size_t next_column = cursor_column_ + count_param();
      cursor_column_ = columns_ > 0 ? std::min(next_column, columns_ - 1) : next_column;
      return;
    }
    case 'D': {
      const std::size_t count = count_param();
      cursor_column_ = cursor_column_ > count ? cursor_column_ - count : 0;
      return;
    }
    case 'E':
      MoveCursorLocked(cursor_row_ + count_param(), 0);
      return;
    case 'F':
      MoveCursorLocked(CursorRowUpLocked(count_param()), 0);
      return;
    case 'G':
      MoveCursorLocked(cursor_row_, position_param(0));
      return;
    case 'H':
    case 'f':
      MoveCursorLocked(position_param(0) + AddressingOriginRowLocked(), position_param(1));
      return;
    case 'J':
      EraseInDisplayLocked(params.empty() ? 0 : params.front());
      return;
    case 'K':
      EraseInLineLocked(params.empty() ? 0 : params.front());
      return;
    case 'L':
      InsertLinesLocked(count_param());
      return;
    case 'M':
      DeleteLinesLocked(count_param());
      return;
    case 'P':
      DeleteCharactersLocked(count_param());
      return;
    case 'X':
      EraseCharactersLocked(count_param());
      return;
    case '@': {
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      ResizeLineLocked(line, cursor_column_ + 1);
      detail::InsertBlankCells(line, cursor_column_, count_param(), columns_, current_style_);
      return;
    }
    case 'I':
    case 'Z': {
      const std::size_t count = std::min(count_param(), std::max<std::size_t>(1, columns_));
      for (std::size_t i = 0; i < count; ++i) {
        cursor_column_ = final == 'I' ? NextTabStopLocked(cursor_column_)
                                      : PreviousTabStopLocked(cursor_column_);
      }
      return;
    }
    case 'g':
      if (tab_stops_.empty()) {
        ResetTabStopsLocked();
      }
      if (!params.empty() && params.front() == 3) {
        std::fill(tab_stops_.begin(), tab_stops_.end(), false);
      } else if (cursor_column_ < tab_stops_.size()) {
        tab_stops_[cursor_column_] = false;
      }
      return;
    case 'q':
      if (intermediate == ' ') {
        SetCursorStyleLocked(CsiParamOrDefault(params, 0, 1));
      }
      return;
    case 'S':
      ScrollUpLocked(count_param());
      return;
    case 'T':
      ScrollRegionDownLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                             count_param());
      return;
    case 'n':
      if (!params.empty()) {
        if (params.front() == 5) {
          SendBytesLocked("\x1b[0n");
        } else if (params.front() == 6) {
          SendBytesLocked(CursorPositionReport(false, cursor_row_, cursor_column_,
                                               AddressingOriginRowLocked(), columns_));
        }
      }
      return;
    case 'd':
      MoveCursorLocked(position_param(0) + AddressingOriginRowLocked(), cursor_column_);
      return;
    case 'r':
      SetScrollMarginsLocked(params);
      return;
    case 'b':
      RepeatLastGlyphLocked(CsiParamOrDefault(params, 0, 1));
      return;
    case 'h':
    case 'l':
      for (const int mode : params) {
        if (mode == 4) {
          insert_mode_ = final == 'h';
        }
      }
      return;
    case 's':
      SaveCursorLocked();
      return;
    case 'u':
      RestoreCursorLocked();
      return;
    default:
      return;
  }
}

std::size_t TerminalSession::AddressingOriginRowLocked() const {
  return PrimaryScreenTopLocked() + (origin_mode_ ? ActiveScrollRegionTopLocked() : 0);
}

std::size_t TerminalSession::CursorRowUpLocked(std::size_t count) const {
  const std::size_t floor_row = AddressingOriginRowLocked();
  return cursor_row_ >= floor_row + count ? cursor_row_ - count : floor_row;
}

void TerminalSession::HandlePrivateCsiLocked(char final, char intermediate,
                                             const std::vector<int>& params) {
  if (final == 'p' && intermediate == '$') {
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
    return;
  }
  if (final != 'n') {
    return;
  }
  for (const int mode : params) {
    if (mode == 996) {
      SendBytesLocked(util::IsDarkColor(default_background_) ? "\x1b[?997;1n" : "\x1b[?997;2n");
    } else if (mode == 6) {
      SendBytesLocked(CursorPositionReport(true, cursor_row_, cursor_column_,
                                           AddressingOriginRowLocked(), columns_));
    }
  }
}

void TerminalSession::SoftResetLocked() {
  cursor_visible_ = true;
  insert_mode_ = false;
  origin_mode_ = false;
  auto_wrap_mode_ = true;
  application_cursor_keys_mode_ = false;
  current_style_ = TerminalStyle{};
  pen_underline_color_.reset();
  current_extra_ = InternExtraLocked(pen_link_uri_, pen_underline_color_);
  charsets_ = {};
  ResetScrollRegionLocked();
  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  saved_style_ = TerminalStyle{};
  saved_underline_color_.reset();
  saved_origin_mode_ = false;
  saved_charsets_ = {};
}

void TerminalSession::InsertLinesLocked(std::size_t count) {
  if (rows_ > 0) {
    count = std::min(count, rows_);
  }
  EnsureCursorLineExistsLocked();
  if (const auto span = CursorScrollSpan(cursor_row_, PrimaryScreenTopLocked(),
                                         ActiveScrollRegionTopLocked(),
                                         ActiveScrollRegionBottomLocked());
      span.has_value()) {
    ScrollRegionDownLocked(span->top, span->bottom, count);
  }
}

void TerminalSession::DeleteLinesLocked(std::size_t count) {
  EnsureCursorLineExistsLocked();
  if (const auto span = CursorScrollSpan(cursor_row_, PrimaryScreenTopLocked(),
                                         ActiveScrollRegionTopLocked(),
                                         ActiveScrollRegionBottomLocked());
      span.has_value()) {
    ScrollRegionUpLocked(span->top, span->bottom, count);
  }
  EnsureCursorLineExistsLocked();
}

void TerminalSession::DeleteCharactersLocked(std::size_t count) {
  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  if (cursor_column_ < line.cells.size()) {
    const std::size_t erase_end = std::min(line.cells.size(), cursor_column_ + count);
    BreakWideGlyphPairForWriteLocked(line, cursor_column_, erase_end - cursor_column_);
    line.cells.erase(line.cells.begin() + static_cast<std::ptrdiff_t>(cursor_column_),
                     line.cells.begin() + static_cast<std::ptrdiff_t>(erase_end));
  }
  if ((current_style_.background.has_value() || current_style_.has(cell_attr::kInverse)) &&
      columns_ > 0 && cursor_column_ < columns_) {
    const std::size_t vacated = std::min(count, columns_ - cursor_column_);
    ClearLineRangeLocked(line, columns_ - vacated, columns_);
  }
}

void TerminalSession::EraseCharactersLocked(std::size_t count) {
  EnsureCursorLineExistsLocked();
  const std::size_t max_end = columns_ > 0 ? columns_ : lines_[cursor_row_].cells.size();
  const std::size_t erase_end = std::min(cursor_column_ + count, max_end);
  ClearLineRangeLocked(lines_[cursor_row_], cursor_column_, erase_end);
}

void TerminalSession::ScrollUpLocked(std::size_t count) {
  if (use_alternate_screen_ || HasCustomScrollRegionLocked()) {
    ScrollRegionUpLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(), count);
    return;
  }
  const std::size_t screen_rows = std::max<std::size_t>(1, rows_);
  const std::size_t shift = std::min(count, screen_rows);
  const std::size_t screen_end = PrimaryScreenTopLocked() + screen_rows;
  while (lines_.size() < screen_end) {
    lines_.push_back(line_buffer_pool_.Take());
  }
  for (std::size_t i = 0; i < shift; ++i) {
    lines_.push_back(line_buffer_pool_.Take());
    BlankLineToCurrentBackgroundLocked(lines_.back());
  }
  cursor_row_ += shift;
}

void TerminalSession::SetScrollMarginsLocked(const std::vector<int>& params) {
  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  const int terminal_rows_int = static_cast<int>(terminal_rows);
  const int scroll_region_top_param = CsiParamOrDefault(params, 0, 1);
  const int scroll_region_bottom_param =
      params.size() > 1 ? CsiParamOrDefault(params, 1, terminal_rows_int) : terminal_rows_int;
  const std::size_t scroll_region_top =
      static_cast<std::size_t>(std::max(0, scroll_region_top_param - 1));
  const std::size_t scroll_region_bottom =
      static_cast<std::size_t>(std::max(0, scroll_region_bottom_param - 1));
  const std::size_t clamped_bottom = std::min(scroll_region_bottom, terminal_rows - 1);
  if (scroll_region_top >= terminal_rows || scroll_region_top >= clamped_bottom) {
    return;
  }
  scroll_region_top_ = scroll_region_top;
  scroll_region_bottom_ = clamped_bottom;
  MoveCursorLocked(AddressingOriginRowLocked(), 0);
}

void TerminalSession::RepeatLastGlyphLocked(int requested) {
  if (last_glyph_length_ == 0) {
    return;
  }
  const std::size_t count =
      std::min(static_cast<std::size_t>(std::max(1, requested)),
               std::max<std::size_t>(1, rows_) * std::max<std::size_t>(1, columns_));
  const std::array<char, 4> glyph = last_glyph_;
  const std::string_view text(glyph.data(), last_glyph_length_);
  const unsigned char first = static_cast<unsigned char>(glyph[0]);
  if (last_glyph_length_ == 1 && first >= 0x20 && first < 0x7F && columns_ > 0) {
    thread_local std::string row;
    row.assign(std::min(count, columns_), static_cast<char>(first));
    for (std::size_t left = count; left > 0;) {
      const std::string_view chunk(row.data(), std::min(left, row.size()));
      std::size_t written = PutAsciiRunLocked(chunk);
      if (written == 0) {
        PutGlyphLocked(text);
        written = 1;
      }
      left -= written;
    }
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    PutGlyphLocked(text);
  }
}

void TerminalSession::SetCursorStyleLocked(int style) {
  switch (style) {
    case 2:
      cursor_shape_ = CursorShape::Block;
      break;
    case 3:
    case 4:
      cursor_shape_ = CursorShape::Underline;
      break;
    case 5:
    case 6:
      cursor_shape_ = CursorShape::Bar;
      break;
    default:
      cursor_shape_ = CursorShape::Block;
      cursor_blinking_ = true;
      return;
  }
  cursor_blinking_ = style % 2 == 1;
}

}
