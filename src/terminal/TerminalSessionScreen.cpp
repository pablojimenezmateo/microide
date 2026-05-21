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

void TerminalSession::EnsureCursorLineExistsLocked() {
  if (lines_.size() <= cursor_row_) {
    const std::size_t grew = cursor_row_ + 1 - lines_.size();
    lines_.resize(cursor_row_ + 1);
    util::AddPerformanceCounter(util::PerfCounterId::TerminalScrollbackLinesAllocated, grew);
  }
}

void TerminalSession::SaveCursorLocked() {
  saved_cursor_row_ = cursor_row_;
  saved_cursor_column_ = cursor_column_;
}

void TerminalSession::RestoreCursorLocked() {
  MoveCursorLocked(saved_cursor_row_, saved_cursor_column_);
}

void TerminalSession::SaveActiveScreenLocked() {
  ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  screen.lines = lines_;
  screen.cursor_row = cursor_row_;
  screen.cursor_column = cursor_column_;
  screen.saved_cursor_row = saved_cursor_row_;
  screen.saved_cursor_column = saved_cursor_column_;
  screen.scroll_region_top = scroll_region_top_;
  screen.scroll_region_bottom = scroll_region_bottom_;
}

void TerminalSession::RestoreSavedScreenLocked() {
  const ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  lines_ = screen.lines.empty() ? std::deque<TerminalLine>{TerminalLine{}} : screen.lines;
  pending_utf8_sequence_.clear();
  if (use_alternate_screen_) {
    lines_.resize(std::max<std::size_t>(1, rows_));
  }
  cursor_row_ = screen.cursor_row;
  cursor_column_ = screen.cursor_column;
  saved_cursor_row_ = screen.saved_cursor_row;
  saved_cursor_column_ = screen.saved_cursor_column;
  scroll_region_top_ = screen.scroll_region_top;
  scroll_region_bottom_ = screen.scroll_region_bottom;
  if (use_alternate_screen_ && rows_ > 0) {
    cursor_row_ = std::min(cursor_row_, rows_ - 1);
    saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
  }
  ClampScrollRegionLocked();
  EnsureCursorLineExistsLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::ResetScreenLocked(bool fill_rows) {
  lines_.assign(fill_rows ? std::max<std::size_t>(1, rows_) : 1, TerminalLine{});
  pending_utf8_sequence_.clear();
  cursor_row_ = 0;
  cursor_column_ = 0;
  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  ResetScrollRegionLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::SetAlternateScreenLocked(bool enabled, bool clear) {
  if (enabled) {
    SaveActiveScreenLocked();
    if (!use_alternate_screen_) {
      use_alternate_screen_ = true;
      RestoreSavedScreenLocked();
    }
    if (clear || alternate_screen_.lines.empty()) {
      ResetScreenLocked(true);
      SaveActiveScreenLocked();
    }
    return;
  }

  if (!use_alternate_screen_) {
    return;
  }

  SaveActiveScreenLocked();
  use_alternate_screen_ = false;
  RestoreSavedScreenLocked();
}

void TerminalSession::AdvanceCursorRowLocked(bool wrapped_from_previous) {
  if (use_alternate_screen_) {
    const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
    if (lines_.size() < terminal_rows) {
      lines_.resize(terminal_rows);
    }
    const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
    const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
    if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
      if (cursor_row_ == scroll_region_bottom) {
        ScrollRegionUpLocked(scroll_region_top, scroll_region_bottom, 1);
        cursor_column_ = 0;
        EnsureCursorLineExistsLocked();
        lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
        return;
      }
      ++cursor_row_;
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
      return;
    }
    if (cursor_row_ + 1 >= terminal_rows) {
      ScrollRegionUpLocked(0, terminal_rows - 1, 1);
      cursor_row_ = terminal_rows - 1;
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
      return;
    }
  }

  ++cursor_row_;
  cursor_column_ = 0;
  EnsureCursorLineExistsLocked();
  lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
}

void TerminalSession::MoveCursorLocked(std::size_t row, std::size_t column) {
  if (use_alternate_screen_) {
    const std::size_t max_row = origin_mode_ ? ActiveScrollRegionBottomLocked()
                                             : std::max<std::size_t>(1, rows_) - 1;
    const std::size_t min_row = origin_mode_ ? ActiveScrollRegionTopLocked() : 0;
    cursor_row_ = std::clamp(row, min_row, max_row);
  } else {
    cursor_row_ = row;
  }
  cursor_column_ =
      columns_ > 0 ? std::min(column, columns_ - 1) : column;
  EnsureCursorLineExistsLocked();
}

void TerminalSession::PutCharacterLocked(char character) {
  PutGlyphLocked(std::string_view(&character, 1));
}

void TerminalSession::PutGlyphLocked(std::string_view glyph) {
  if (columns_ > 0 && cursor_column_ >= columns_) {
    if (auto_wrap_mode_) {
      AdvanceCursorRowLocked(true);
    } else {
      cursor_column_ = columns_ - 1;
    }
  }

  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  ResizeLineLocked(line, cursor_column_);
  if (cursor_column_ == line.cells.size()) {
    line.cells.push_back(MakeUtf8TerminalCell(glyph, current_style_));
  } else {
    line.cells[cursor_column_] = MakeUtf8TerminalCell(glyph, current_style_);
  }
  ++cursor_column_;
}

void TerminalSession::ResizeLineLocked(TerminalLine& line, std::size_t size) {
  if (line.cells.size() < size) {
    line.cells.resize(size, MakeAsciiTerminalCell(' ', TerminalStyle{}));
  }
}

void TerminalSession::ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end) {
  if (start >= end) {
    return;
  }

  ResizeLineLocked(line, end);
  for (std::size_t i = start; i < end; ++i) {
    line.cells[i] = MakeAsciiTerminalCell(' ', current_style_);
  }
}

void TerminalSession::EraseInLineLocked(int mode) {
  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  switch (mode) {
    case 1:
      ClearLineRangeLocked(line, 0, cursor_column_ + 1);
      break;
    case 2:
      ClearLineRangeLocked(line, 0, std::max<std::size_t>(line.cells.size(), cursor_column_ + 1));
      break;
    case 0:
    default:
      if (cursor_column_ < line.cells.size()) {
        ClearLineRangeLocked(line, cursor_column_, line.cells.size());
      }
      break;
  }
}

void TerminalSession::EraseInDisplayLocked(int mode) {
  EnsureCursorLineExistsLocked();
  switch (mode) {
    case 1:
      for (std::size_t row = 0; row < cursor_row_; ++row) {
        lines_[row].cells.clear();
      }
      EraseInLineLocked(1);
      break;
    case 2:
      lines_.assign(use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                          : std::max<std::size_t>(1, cursor_row_ + 1),
                    TerminalLine{});
      break;
    case 0:
    default:
      EraseInLineLocked(0);
      for (std::size_t row = cursor_row_ + 1; row < lines_.size(); ++row) {
        lines_[row].cells.clear();
      }
      break;
  }
}

void TerminalSession::ResetScrollRegionLocked() {
  scroll_region_top_ = 0;
  scroll_region_bottom_ = rows_ > 0 ? rows_ - 1 : 0;
}

void TerminalSession::ClampScrollRegionLocked() {
  if (rows_ == 0) {
    scroll_region_top_ = 0;
    scroll_region_bottom_ = 0;
    return;
  }
  scroll_region_top_ = std::min(scroll_region_top_, rows_ - 1);
  scroll_region_bottom_ = std::min(scroll_region_bottom_, rows_ - 1);
  if (scroll_region_bottom_ < scroll_region_top_) {
    ResetScrollRegionLocked();
  }
}

std::size_t TerminalSession::ActiveScrollRegionTopLocked() const {
  if (rows_ == 0) {
    return 0;
  }
  return std::min(scroll_region_top_, rows_ - 1);
}

std::size_t TerminalSession::ActiveScrollRegionBottomLocked() const {
  if (rows_ == 0) {
    return 0;
  }
  return std::clamp(scroll_region_bottom_, ActiveScrollRegionTopLocked(), rows_ - 1);
}

void TerminalSession::ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count) {
  if (!use_alternate_screen_ || rows_ == 0) {
    return;
  }

  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  if (lines_.size() < terminal_rows) {
    lines_.resize(terminal_rows);
  }

  const std::size_t clamped_top = std::min(top, terminal_rows - 1);
  const std::size_t clamped_bottom = std::clamp(bottom, clamped_top, terminal_rows - 1);
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, begin + static_cast<std::ptrdiff_t>(shift), end);
  std::fill(end - static_cast<std::ptrdiff_t>(shift), end, TerminalLine{});
}

void TerminalSession::ScrollRegionDownLocked(std::size_t top,
                                             std::size_t bottom,
                                             std::size_t count) {
  if (!use_alternate_screen_ || rows_ == 0) {
    return;
  }

  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  if (lines_.size() < terminal_rows) {
    lines_.resize(terminal_rows);
  }

  const std::size_t clamped_top = std::min(top, terminal_rows - 1);
  const std::size_t clamped_bottom = std::clamp(bottom, clamped_top, terminal_rows - 1);
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, end - static_cast<std::ptrdiff_t>(shift), end);
  std::fill(begin, begin + static_cast<std::ptrdiff_t>(shift), TerminalLine{});
}

void TerminalSession::TrimScrollbackLocked() {
  const std::size_t max_lines = use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                                      : kMaxScrollbackLines +
                                                            std::max<std::size_t>(1, rows_);
  // Coalesce trims: only act once we are 25 % above the target, then trim all
  // the way down to `max_lines`. The previous implementation trimmed on every
  // newline once the cap was reached, calling `vector::erase(begin, …)` ~6 700
  // times per `terminal_scroll_long_output` iteration; each erase walked the
  // remaining N tail elements. Coalescing cuts the call count by an order of
  // magnitude (round-4 Finding 3).
  const std::size_t trim_high_watermark = max_lines + max_lines / 4 + 1;
  if (lines_.size() < trim_high_watermark) {
    return;
  }

  const std::size_t trim_count = lines_.size() - max_lines;
  util::AddPerformanceCounter(util::PerfCounterId::TerminalTrimScrollbackCalls);
  util::AddPerformanceCounter(util::PerfCounterId::TerminalTrimScrollbackLines, trim_count);
  lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
  cursor_row_ = cursor_row_ > trim_count ? cursor_row_ - trim_count : 0;
  saved_cursor_row_ = saved_cursor_row_ > trim_count ? saved_cursor_row_ - trim_count : 0;
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
}
}  // namespace microide::terminal
