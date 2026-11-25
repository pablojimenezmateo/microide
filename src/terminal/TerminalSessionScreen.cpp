#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalSessionEscapeInternal.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalOscClipboard.h"
#include "platform/TerminalBackend.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace microide::terminal {

void TerminalSession::EnsureCursorLineExistsLocked() {
  if (lines_.size() <= cursor_row_) {
    const std::size_t grew = cursor_row_ + 1 - lines_.size();
    for (std::size_t i = 0; i < grew; ++i) {
      lines_.push_back(line_buffer_pool_.Take());
    }
    util::AddPerformanceCounter(util::PerfCounterId::TerminalScrollbackLinesAllocated, grew);
  }
}

void TerminalSession::SaveCursorLocked() {
  const std::size_t screen_top = PrimaryScreenTopLocked();
  saved_cursor_row_ = std::min(cursor_row_ > screen_top ? cursor_row_ - screen_top : 0,
                               std::max<std::size_t>(1, rows_) - 1);
  ClampPendingWrapColumn(cursor_column_, columns_);
  saved_cursor_column_ = cursor_column_;
  saved_style_ = current_style_;
  saved_underline_color_ = pen_underline_color_;
  saved_origin_mode_ = origin_mode_;
  saved_charsets_ = charsets_;
}

void TerminalSession::RestoreCursorLocked() {
  current_style_ = saved_style_;
  pen_underline_color_ = saved_underline_color_;
  current_extra_ = InternExtraLocked(pen_link_uri_, pen_underline_color_);
  origin_mode_ = saved_origin_mode_;
  charsets_ = saved_charsets_;
  MoveCursorLocked(PrimaryScreenTopLocked() + saved_cursor_row_, saved_cursor_column_);
}

void TerminalSession::SaveActiveScreenMetadataLocked(TerminalScreenState& screen) {
  screen.cursor_row = cursor_row_;
  screen.cursor_column = cursor_column_;
  screen.saved_cursor_row = saved_cursor_row_;
  screen.saved_cursor_column = saved_cursor_column_;
  screen.saved_style = saved_style_;
  screen.saved_underline_color = saved_underline_color_;
  screen.saved_origin_mode = saved_origin_mode_;
  screen.saved_charsets = saved_charsets_;
  screen.scroll_region_top = scroll_region_top_;
  screen.scroll_region_bottom = scroll_region_bottom_;
}

void TerminalSession::SaveActiveScreenLocked() {
  TerminalScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  screen.lines = lines_;
  SaveActiveScreenMetadataLocked(screen);
}

void TerminalSession::RestoreSavedScreenLocked() {
  TerminalScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  if (screen.lines.empty()) {
    lines_ = std::deque<TerminalLine>{TerminalLine{}};
  } else {
    lines_ = std::move(screen.lines);
  }
  pending_utf8_sequence_.clear();
  if (use_alternate_screen_) {
    lines_.resize(std::max<std::size_t>(1, rows_));
  }
  cursor_row_ = screen.cursor_row;
  cursor_column_ = screen.cursor_column;
  saved_cursor_row_ = screen.saved_cursor_row;
  saved_cursor_column_ = screen.saved_cursor_column;
  saved_style_ = screen.saved_style;
  saved_underline_color_ = screen.saved_underline_color;
  saved_origin_mode_ = screen.saved_origin_mode;
  saved_charsets_ = screen.saved_charsets;
  scroll_region_top_ = screen.scroll_region_top;
  scroll_region_bottom_ = screen.scroll_region_bottom;
  if (rows_ > 0) {
    saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
  }
  if (use_alternate_screen_ && rows_ > 0) {
    cursor_row_ = std::min(cursor_row_, rows_ - 1);
  } else {
    const std::size_t max_row = max_scrollback_lines_ + std::max<std::size_t>(1, rows_);
    cursor_row_ = std::min(cursor_row_, max_row);
  }
  ClampScrollRegionLocked();
  EnsureCursorLineExistsLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::ResetScreenLocked(bool fill_rows) {
  lines_.assign(fill_rows ? std::max<std::size_t>(1, rows_) : 1, TerminalLine{});
  pointer_ = {};
  pending_utf8_sequence_.clear();
  cursor_row_ = 0;
  cursor_column_ = 0;
  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  ResetScrollRegionLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::SetAlternateScreenLocked(bool enabled, bool clear) {
  pointer_.hover.Reset();
  if (enabled) {
    bool should_clear = clear;
    if (!use_alternate_screen_) {
      should_clear = should_clear || alternate_screen_.lines.empty();
      SaveActiveScreenMetadataLocked(primary_screen_);
      primary_screen_.lines = std::move(lines_);
      use_alternate_screen_ = true;
      RestoreSavedScreenLocked();
    } else {
      SaveActiveScreenLocked();
      should_clear = should_clear || alternate_screen_.lines.empty();
    }
    if (should_clear) {
      ResetScreenLocked(true);
      SaveActiveScreenLocked();
    }
    return;
  }

  if (!use_alternate_screen_) {
    return;
  }

  SaveActiveScreenMetadataLocked(alternate_screen_);
  alternate_screen_.lines = std::move(lines_);
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
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      return;
    }
  } else if (HasCustomScrollRegionLocked()) {
    const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
    const std::size_t screen_top = PrimaryScreenTopLocked();
    const std::size_t rel_top = ActiveScrollRegionTopLocked();
    const std::size_t rel_bottom = ActiveScrollRegionBottomLocked();
    const std::size_t region_top_abs = screen_top + rel_top;
    const std::size_t region_bottom_abs = screen_top + rel_bottom;
    if (cursor_row_ >= region_top_abs && cursor_row_ <= region_bottom_abs) {
      if (cursor_row_ == region_bottom_abs) {
        ScrollRegionUpLocked(rel_top, rel_bottom, 1);
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
    if (cursor_row_ > region_bottom_abs && cursor_row_ + 1 >= screen_top + terminal_rows) {
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      return;
    }
  }

  ++cursor_row_;
  cursor_column_ = 0;
  const bool row_existed = cursor_row_ < lines_.size();
  EnsureCursorLineExistsLocked();
  if (!row_existed || wrapped_from_previous) {
    lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
  }
}

std::size_t TerminalSession::PrimaryScreenTopLocked() const {
  if (use_alternate_screen_) {
    return 0;
  }
  const std::size_t visible = std::max<std::size_t>(1, rows_);
  return lines_.size() > visible ? lines_.size() - visible : 0;
}

void TerminalSession::MoveCursorLocked(std::size_t row, std::size_t column) {
  const std::size_t screen_top = PrimaryScreenTopLocked();
  const std::size_t min_row = origin_mode_ ? screen_top + ActiveScrollRegionTopLocked() : 0;
  const std::size_t max_row = screen_top + (origin_mode_ ? ActiveScrollRegionBottomLocked()
                                                         : std::max<std::size_t>(1, rows_) - 1);
  cursor_row_ = std::clamp(row, min_row, max_row);
  cursor_column_ = columns_ > 0 ? std::min(column, columns_ - 1) : column;
  EnsureCursorLineExistsLocked();
}

void TerminalSession::PutGlyphLocked(std::string_view glyph) {
  const bool ascii = glyph.size() == 1 && static_cast<unsigned char>(glyph.front()) < 0x80;
  std::string_view effective_glyph = glyph;
  int width;
  if (ascii) {
    width = 1;
    if (charsets_.DecGraphicsActive()) {
      if (const std::string_view mapped = DecSpecialGraphicsGlyph(static_cast<unsigned char>(glyph.front()));
          !mapped.empty()) {
        effective_glyph = mapped;
      }
    }
  } else {
    const char32_t codepoint = util::DecodeUtf8Codepoint(glyph);
    if (codepoint == 0xFFFD) {
      effective_glyph = util::kUtf8ReplacementChar;
    }
    width = util::CodepointDisplayWidth(codepoint);
  }

  if (width == 0) {
    if (cursor_column_ == 0) {
      return;
    }
    EnsureCursorLineExistsLocked();
    auto& line = lines_[cursor_row_];
    std::size_t base = cursor_column_ - 1;
    if (base > 0 && base < line.cells.size() && line.cells[base].style.wide_trailing()) {
      --base;
    }
    if (base < line.cells.size()) {
      TerminalCell& cell = line.cells[base];
      if (cell.length > 0 && cell.length + effective_glyph.size() <= cell.bytes.size()) {
        for (char byte : effective_glyph) {
          cell.bytes[cell.length++] = byte;
        }
      }
    }
    return;
  }

  std::size_t advance = width == 2 ? 2 : 1;
  if (columns_ > 0 && advance > columns_) {
    advance = columns_;
  }

  if (columns_ > 0 && cursor_column_ + advance > columns_) {
    if (auto_wrap_mode_) {
      AdvanceCursorRowLocked(true);
    } else {
      cursor_column_ = columns_ - 1;
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      ResizeLineLocked(line, cursor_column_ + 1);
      BreakWideGlyphPairForWriteLocked(line, cursor_column_, 1);
      line.cells[cursor_column_] = MakeUtf8TerminalCell(effective_glyph, current_style_);
      line.cells[cursor_column_].extra = current_extra_;
      return;
    }
  }

  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  if (insert_mode_) {
    detail::InsertBlankCells(line, cursor_column_, advance, columns_, current_style_);
  }
  ResizeLineLocked(line, cursor_column_ + advance);
  BreakWideGlyphPairForWriteLocked(line, cursor_column_, advance);
  if (effective_glyph.size() <= last_glyph_.size()) {
    std::copy(effective_glyph.begin(), effective_glyph.end(), last_glyph_.begin());
    last_glyph_length_ = static_cast<std::uint8_t>(effective_glyph.size());
  }
  line.cells[cursor_column_] = MakeUtf8TerminalCell(effective_glyph, current_style_);
  line.cells[cursor_column_].extra = current_extra_;
  if (advance == 2) {
    TerminalCell spacer;
    spacer.style = current_style_;
    spacer.style.set(cell_attr::kWideTrailing, true);
    spacer.extra = current_extra_;
    line.cells[cursor_column_ + 1] = spacer;
  }
  cursor_column_ += advance;
}

std::size_t TerminalSession::PutAsciiRunLocked(std::string_view run) {
  std::size_t length = 0;
  while (length < run.size()) {
    const auto byte = static_cast<unsigned char>(run[length]);
    if (byte < 0x20 || byte >= 0x7F) {
      break;
    }
    ++length;
  }
  if (length == 0 || insert_mode_ || charsets_.DecGraphicsActive()) {
    return 0;
  }
  if (columns_ > 0) {
    if (cursor_column_ >= columns_) {
      return 0;
    }
    length = std::min(length, columns_ - cursor_column_);
  }

  EnsureCursorLineExistsLocked();
  TerminalLine& line = lines_[cursor_row_];
  ResizeLineLocked(line, cursor_column_ + length);
  BreakWideGlyphPairForWriteLocked(line, cursor_column_, length);

  TerminalCell cell;
  cell.style = current_style_;
  cell.extra = current_extra_;
  cell.length = 1;
  for (std::size_t i = 0; i < length; ++i) {
    cell.bytes[0] = run[i];
    line.cells[cursor_column_ + i] = cell;
  }
  cursor_column_ += length;
  last_glyph_[0] = run[length - 1];
  last_glyph_length_ = 1;
  return length;
}

void TerminalSession::ResizeLineLocked(TerminalLine& line, std::size_t size) {
  if (line.cells.size() >= size) {
    return;
  }
  if (line.cells.capacity() < size) {
    line.cells.reserve(std::max(size, columns_));
  }
  line.cells.resize(size, MakeAsciiTerminalCell(' ', TerminalStyle{}));
}

void TerminalSession::BreakWideGlyphPairForWriteLocked(TerminalLine& line, std::size_t start,
                                                       std::size_t advance) {
  if (advance == 0 || start >= line.cells.size()) {
    return;
  }
  if (line.cells[start].style.wide_trailing() && start > 0) {
    line.cells[start - 1] = MakeAsciiTerminalCell(' ', current_style_);
  }
  const std::size_t last = start + advance - 1;
  if (last + 1 < line.cells.size() && line.cells[last + 1].style.wide_trailing()) {
    line.cells[last + 1] = MakeAsciiTerminalCell(' ', current_style_);
  }
}

void TerminalSession::ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end) {
  if (start >= end) {
    return;
  }

  ResizeLineLocked(line, end);
  BreakWideGlyphPairForWriteLocked(line, start, end - start);
  for (std::size_t i = start; i < end; ++i) {
    line.cells[i] = MakeAsciiTerminalCell(' ', current_style_);
  }
}

void TerminalSession::EraseInLineLocked(int mode) {
  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  switch (mode) {
    case 1: {
      const std::size_t end =
          columns_ > 0 ? std::min<std::size_t>(cursor_column_ + 1, columns_) : cursor_column_ + 1;
      ClearLineRangeLocked(line, 0, end);
      break;
    }
    case 2: {
      const std::size_t end =
          columns_ > 0 ? std::max<std::size_t>(line.cells.size(), columns_) : line.cells.size();
      ClearLineRangeLocked(line, 0, end);
      break;
    }
    case 0:
    default:
      if (cursor_column_ < line.cells.size()) {
        ClearLineRangeLocked(line, cursor_column_, line.cells.size());
      }
      break;
  }
}

void TerminalSession::BlankLineToCurrentBackgroundLocked(TerminalLine& line) {
  const bool erases_to_default_background =
      !current_style_.background.has_value() && !current_style_.has(cell_attr::kInverse);
  if (erases_to_default_background || columns_ == 0) {
    line = TerminalLine{};
    return;
  }
  line.cells.assign(columns_, MakeAsciiTerminalCell(' ', current_style_));
}

void TerminalSession::EraseInDisplayLocked(int mode) {
  EnsureCursorLineExistsLocked();
  switch (mode) {
    case 1: {
      const std::size_t top = PrimaryScreenTopLocked();
      for (std::size_t row = top; row < cursor_row_ && row < lines_.size(); ++row) {
        BlankLineToCurrentBackgroundLocked(lines_[row]);
      }
      EraseInLineLocked(1);
      break;
    }
    case 2:
      if (use_alternate_screen_) {
        lines_.assign(std::max<std::size_t>(1, rows_), TerminalLine{});
        for (auto& row : lines_) {
          BlankLineToCurrentBackgroundLocked(row);
        }
      } else {
        const std::size_t visible = std::max<std::size_t>(1, rows_);
        while (lines_.size() < visible) {
          lines_.push_back(TerminalLine{});
        }
        for (std::size_t row = lines_.size() - visible; row < lines_.size(); ++row) {
          BlankLineToCurrentBackgroundLocked(lines_[row]);
        }
      }
      break;
    case 3: {
      if (!use_alternate_screen_) {
        const std::size_t visible = std::max<std::size_t>(1, rows_);
        if (lines_.size() > visible) {
          const std::size_t trim_count = lines_.size() - visible;
          line_buffer_pool_.RecycleRange(
              lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
          lines_.erase(lines_.begin(),
                       lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
          cursor_row_ = cursor_row_ > trim_count ? cursor_row_ - trim_count : 0;
          scrollback_trim_total_ += static_cast<std::uint64_t>(trim_count);
        }
      }
      break;
    }
    case 0:
    default:
      EraseInLineLocked(0);
      for (std::size_t row = cursor_row_ + 1; row < lines_.size(); ++row) {
        BlankLineToCurrentBackgroundLocked(lines_[row]);
      }
      break;
  }
}

void TerminalSession::ResetScrollRegionLocked() {
  scroll_region_top_ = 0;
  scroll_region_bottom_ = rows_ > 0 ? rows_ - 1 : 0;
}

void TerminalSession::ResetEmulationStateLocked() {
  child_pid_ = -1;
  running_ = false;
  stop_requested_ = false;

  current_style_ = TerminalStyle{};
  pen_link_uri_.clear();
  pen_underline_color_.reset();
  current_extra_ = 0;
  extras_.Clear();
  escape_sequence_buffer_.clear();
  pending_utf8_sequence_.clear();
  escape_mode_ = EscapeMode::None;
  osc_escape_pending_ = false;
  pending_clipboard_text_.reset();

  use_alternate_screen_ = false;
  ResetTerminalModesLocked();
  reported_working_directory_.clear();
  tab_stops_.clear();

  primary_screen_ = TerminalScreenState{};
  alternate_screen_ = TerminalScreenState{};
  cursor_row_ = 0;
  cursor_column_ = 0;
  ResetScrollRegionLocked();
}

void TerminalSession::ResetTerminalModesLocked() {
  mouse_ = {};
  application_cursor_keys_mode_ = false;
  origin_mode_ = false;
  auto_wrap_mode_ = true;
  insert_mode_ = false;
  last_glyph_length_ = 0;
  charsets_ = {};
  bracketed_paste_mode_ = false;
  focus_event_mode_ = false;
  cursor_visible_ = true;
  screen_reverse_video_ = false;
  color_scheme_updates_ = false;
  current_style_ = TerminalStyle{};
  pen_link_uri_.clear();
  pen_underline_color_.reset();
  current_extra_ = 0;

  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  saved_style_ = TerminalStyle{};
  saved_underline_color_.reset();
  saved_origin_mode_ = false;
  saved_charsets_ = {};

  kitty_keyboard_flags_ = 0;
  kitty_keyboard_stack_.clear();
  synchronized_output_ = false;
  sync_suppressed_wakes_ = 0;
  sync_frame_completed_ = false;
  cursor_shape_ = CursorShape::Block;
  cursor_blinking_ = true;
}

void TerminalSession::ReseedForStartLocked(const std::filesystem::path& working_directory,
                                           std::string launch_label) {
  working_directory_ = working_directory;
  default_launch_label_ = std::move(launch_label);
  launch_label_ = default_launch_label_;
  lines_ = {TerminalLine{}};
  rows_ = 24;
  columns_ = 80;
  ResetEmulationStateLocked();
  wake_event_pending_ = false;
  snapshot_generation_.Reset(1);
}

void TerminalSession::ResetTabStopsLocked() {
  const std::size_t width = std::max<std::size_t>(1, columns_);
  tab_stops_.assign(width, false);
  for (std::size_t column = 8; column < width; column += 8) {
    tab_stops_[column] = true;
  }
}

void TerminalSession::ResizeTabStopsLocked() {
  const std::size_t width = std::max<std::size_t>(1, columns_);
  if (tab_stops_.empty()) {
    ResetTabStopsLocked();
    return;
  }
  const std::size_t old_size = tab_stops_.size();
  tab_stops_.resize(width, false);
  for (std::size_t column = 8; column < width; column += 8) {
    if (column >= old_size) {
      tab_stops_[column] = true;
    }
  }
}

std::size_t TerminalSession::NextTabStopLocked(std::size_t column) const {
  const std::size_t width = std::max<std::size_t>(1, columns_);
  if (column >= width - 1) {
    return column;
  }
  if (tab_stops_.empty()) {
    return std::min(((column / 8) + 1) * 8, width - 1);
  }
  for (std::size_t candidate = column + 1; candidate < width; ++candidate) {
    if (candidate < tab_stops_.size() && tab_stops_[candidate]) {
      return candidate;
    }
  }
  return width - 1;
}

std::size_t TerminalSession::PreviousTabStopLocked(std::size_t column) const {
  if (column == 0) {
    return 0;
  }
  if (tab_stops_.empty()) {
    return ((column - 1) / 8) * 8;
  }
  for (std::size_t candidate = column; candidate-- > 0;) {
    if (candidate < tab_stops_.size() && tab_stops_[candidate]) {
      return candidate;
    }
  }
  return 0;
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

bool TerminalSession::HasCustomScrollRegionLocked() const {
  if (rows_ == 0) {
    return false;
  }
  return ActiveScrollRegionTopLocked() != 0 || ActiveScrollRegionBottomLocked() != rows_ - 1;
}

TerminalSession::ScrollRotation TerminalSession::ResolveScrollRegionLocked(std::size_t top,
                                                                           std::size_t bottom,
                                                                           std::size_t count) {
  if (rows_ == 0) {
    return {};
  }
  const std::size_t terminal_rows = rows_;
  const std::size_t screen_top = use_alternate_screen_ ? 0 : PrimaryScreenTopLocked();
  if (lines_.size() < screen_top + terminal_rows) {
    lines_.resize(screen_top + terminal_rows);
  }
  const std::size_t rel_top = std::min(top, terminal_rows - 1);
  const std::size_t rel_bottom = std::clamp(bottom, rel_top, terminal_rows - 1);
  return ScrollRotation{
      .first_row = rel_top + screen_top,
      .last_row = rel_bottom + screen_top,
      .shift = std::min(count, rel_bottom - rel_top + 1),
  };
}

void TerminalSession::ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count) {
  const ScrollRotation rotation = ResolveScrollRegionLocked(top, bottom, count);
  if (rotation.shift == 0) {
    return;
  }
  const std::size_t shift = rotation.shift;
  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(rotation.first_row);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(rotation.last_row + 1);
  std::rotate(begin, begin + static_cast<std::ptrdiff_t>(shift), end);
  for (auto it = end - static_cast<std::ptrdiff_t>(shift); it != end; ++it) {
    BlankLineToCurrentBackgroundLocked(*it);
  }
}

void TerminalSession::ScrollRegionDownLocked(std::size_t top,
                                             std::size_t bottom,
                                             std::size_t count) {
  const ScrollRotation rotation = ResolveScrollRegionLocked(top, bottom, count);
  if (rotation.shift == 0) {
    return;
  }
  const std::size_t shift = rotation.shift;
  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(rotation.first_row);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(rotation.last_row + 1);
  std::rotate(begin, end - static_cast<std::ptrdiff_t>(shift), end);
  for (auto it = begin; it != begin + static_cast<std::ptrdiff_t>(shift); ++it) {
    BlankLineToCurrentBackgroundLocked(*it);
  }
}

void TerminalSession::TrimScrollbackLocked() {
  const std::size_t max_lines = use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                                      : max_scrollback_lines_ +
                                                            std::max<std::size_t>(1, rows_);
  const std::size_t trim_high_watermark = max_lines + max_lines / 4 + 1;
  if (lines_.size() < trim_high_watermark) {
    return;
  }

  const std::size_t trim_count = lines_.size() - max_lines;
  util::AddPerformanceCounter(util::PerfCounterId::TerminalTrimScrollbackCalls);
  util::AddPerformanceCounter(util::PerfCounterId::TerminalTrimScrollbackLines, trim_count);
  line_buffer_pool_.RecycleRange(lines_.begin(),
                                 lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
  lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
  cursor_row_ = cursor_row_ > trim_count ? cursor_row_ - trim_count : 0;
  scrollback_trim_total_ += static_cast<std::uint64_t>(trim_count);
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
}
std::string_view DecSpecialGraphicsGlyph(unsigned char byte) {
  static constexpr std::string_view kGlyphs[] = {
      "\u00a0", "\u25c6", "\u2592", "\u2409", "\u240c", "\u240d", "\u240a", "\u00b0",
      "\u00b1", "\u2424", "\u240b", "\u2518", "\u2510", "\u250c", "\u2514", "\u253c",
      "\u23ba", "\u23bb", "\u2500", "\u23bc", "\u23bd", "\u251c", "\u2524", "\u2534",
      "\u252c", "\u2502", "\u2264", "\u2265", "\u03c0", "\u2260", "\u00a3", "\u00b7",
  };
  static_assert(std::size(kGlyphs) == 0x7F - 0x5F);
  return byte >= 0x5F && byte < 0x7F ? kGlyphs[byte - 0x5F] : std::string_view();
}

namespace detail {

void InsertBlankCells(TerminalLine& line, std::size_t at, std::size_t count, std::size_t columns,
                      const TerminalStyle& style) {
  if (at >= line.cells.size() || count == 0) {
    return;
  }
  if (line.cells[at].style.wide_trailing() && at > 0) {
    line.cells[at - 1] = MakeAsciiTerminalCell(' ', style);
    line.cells[at] = MakeAsciiTerminalCell(' ', style);
  }
  if (columns > 0) {
    count = std::min(count, columns);
  }
  line.cells.insert(line.cells.begin() + static_cast<std::ptrdiff_t>(at), count,
                    MakeAsciiTerminalCell(' ', style));
  if (columns > 0 && line.cells.size() > columns) {
    if (line.cells[columns].style.wide_trailing()) {
      line.cells[columns - 1] = MakeAsciiTerminalCell(' ', style);
    }
    line.cells.resize(columns);
  }
}

}
}
