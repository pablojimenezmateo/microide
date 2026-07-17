#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
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
  saved_style_ = current_style_;
  saved_origin_mode_ = origin_mode_;
}

void TerminalSession::RestoreCursorLocked() {
  // DECRC restores the graphic rendition and origin mode saved by DECSC before the
  // cursor move (origin mode affects how MoveCursorLocked clamps the row).
  current_style_ = saved_style_;
  origin_mode_ = saved_origin_mode_;
  MoveCursorLocked(saved_cursor_row_, saved_cursor_column_);
}

void TerminalSession::SaveActiveScreenMetadataLocked(ScreenState& screen) {
  // Persist only the cheap scalar cursor / scroll-region state. The line buffer
  // is handled separately by the caller so the alternate-screen transition can
  // move the (potentially multi-MB) scrollback instead of copying it.
  screen.cursor_row = cursor_row_;
  screen.cursor_column = cursor_column_;
  screen.saved_cursor_row = saved_cursor_row_;
  screen.saved_cursor_column = saved_cursor_column_;
  screen.saved_style = saved_style_;
  screen.saved_origin_mode = saved_origin_mode_;
  screen.scroll_region_top = scroll_region_top_;
  screen.scroll_region_bottom = scroll_region_bottom_;
}

void TerminalSession::SaveActiveScreenLocked() {
  ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  screen.lines = lines_;
  SaveActiveScreenMetadataLocked(screen);
}

void TerminalSession::RestoreSavedScreenLocked() {
  // Move the saved buffer into the active grid: the backing store is only read
  // here and is rewritten on the next alternate-screen enter, so there is never
  // a second live reference to copy for. `SaveActiveScreenLocked`/`SetAlternate-
  // ScreenLocked` are the sole callers (no aliasing concern).
  ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
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
  saved_origin_mode_ = screen.saved_origin_mode;
  scroll_region_top_ = screen.scroll_region_top;
  scroll_region_bottom_ = screen.scroll_region_bottom;
  if (use_alternate_screen_ && rows_ > 0) {
    cursor_row_ = std::min(cursor_row_, rows_ - 1);
    saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
  } else {
    // TD-2026-07-17-067: the primary restore trusts the saved cursor_row_ and then
    // allocates lines up to it via EnsureCursorLineExistsLocked. An oversized or
    // corrupted saved row would balloon the deque, so clamp to the same scrollback
    // ceiling MoveCursorLocked enforces (max_scrollback_lines_ + rows_) — exactly
    // what TrimScrollbackLocked would collapse to anyway. The alternate branch above
    // already clamps to rows_ - 1. This clamp is deliberately NOT inside
    // EnsureCursorLineExistsLocked: the normal scroll path grows the deque past this
    // ceiling within one output chunk and relies on end-of-chunk trimming.
    const std::size_t max_row = max_scrollback_lines_ + std::max<std::size_t>(1, rows_);
    cursor_row_ = std::min(cursor_row_, max_row);
    saved_cursor_row_ = std::min(saved_cursor_row_, max_row);
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
    bool should_clear = clear;
    if (!use_alternate_screen_) {
      // Whether the alternate screen has never been initialized. Captured before
      // the move below empties the backing store (the move replaces the copy the
      // original implementation relied on to read this after the restore).
      should_clear = should_clear || alternate_screen_.lines.empty();
      // Hand the primary scrollback to its backing store by move (O(1)); the
      // active grid is about to be replaced by the alternate buffer anyway.
      SaveActiveScreenMetadataLocked(primary_screen_);
      primary_screen_.lines = std::move(lines_);
      use_alternate_screen_ = true;
      RestoreSavedScreenLocked();  // moves alternate_screen_.lines into lines_
    } else {
      // Already on the alternate screen: a redundant enter. Snapshot the current
      // alternate grid so a later restore sees the latest contents.
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

  // Hand the alternate grid back to its store by move, then move the primary
  // scrollback back into the active grid.
  SaveActiveScreenMetadataLocked(alternate_screen_);
  alternate_screen_.lines = std::move(lines_);
  use_alternate_screen_ = false;
  RestoreSavedScreenLocked();  // moves primary_screen_.lines into lines_
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
      // The cursor is OUTSIDE the scroll region (a custom DECSTBM region that does
      // not reach the physical bottom) and on the last physical row. Per DEC/xterm,
      // IND/LF here does NOT scroll — only motion inside the region scrolls. The
      // previous ScrollRegionUpLocked(0, rows-1, 1) scrolled the WHOLE screen,
      // corrupting a header/status-split layout. Clamp at the bottom instead.
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      return;
    }
  } else if (HasCustomScrollRegionLocked()) {
    // Primary buffer with a custom DECSTBM region (e.g. a bottom-status-line
    // program that stays off the alternate screen). Respect the region instead of
    // accumulating scrollback across it. Without a custom region we fall through
    // to the normal grow-scrollback path below (the common, well-tested case).
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
    // Cursor below the region: clamp at the physical screen bottom (no scroll),
    // matching the alt-screen rule above.
    if (cursor_row_ > region_bottom_abs && cursor_row_ + 1 >= screen_top + terminal_rows) {
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      return;
    }
    // Otherwise (above the region, or below it but not yet at the bottom) fall
    // through to the plain advance.
  }

  ++cursor_row_;
  cursor_column_ = 0;
  // Stamp the soft-wrap flag when the row is freshly created, OR whenever this
  // advance is itself a soft wrap (auto-wrap at the right margin): the row the
  // wrapped glyph lands on is, by definition, a continuation of the previous row
  // even if that row already existed (cursor was moved up into mid-screen, then a
  // long line was printed that wraps onto an existing row below). Only a *hard* LF
  // (wrapped_from_previous == false) landing on a pre-existing row is left
  // untouched, so it never relabels an existing soft-wrapped continuation as a
  // hard boundary. Getting this wrong split the wrapped logical line in two for
  // reflow / command capture / selection-by-logical-line.
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
  if (use_alternate_screen_) {
    const std::size_t max_row = origin_mode_ ? ActiveScrollRegionBottomLocked()
                                             : std::max<std::size_t>(1, rows_) - 1;
    const std::size_t min_row = origin_mode_ ? ActiveScrollRegionTopLocked() : 0;
    cursor_row_ = std::clamp(row, min_row, max_row);
  } else {
    // Primary screen: the cursor may descend into scrollback, but an absolute /
    // additive move (CUP, CUD `CSI B`, CNL `CSI E`, VPA) must not balloon the
    // line deque past the scrollback ceiling before the end-of-chunk trim runs.
    // Without this clamp a stream of `\x1b[65535B` accumulates into `cursor_row_`
    // (a persistent member) and `EnsureCursorLineExistsLocked` resizes `lines_`
    // to tens of millions of entries within one 4 KiB read — an OOM/crash on the
    // reader thread. `max_scrollback_lines_ + rows_` is exactly what
    // TrimScrollbackLocked would collapse the deque to anyway, so clamping here
    // never clips legitimately reachable content. Mirrors the `CSI L` clamp.
    const std::size_t primary_max_row =
        max_scrollback_lines_ + std::max<std::size_t>(1, rows_);
    cursor_row_ = std::min(row, primary_max_row);
  }
  cursor_column_ =
      columns_ > 0 ? std::min(column, columns_ - 1) : column;
  EnsureCursorLineExistsLocked();
}

void TerminalSession::PutCharacterLocked(char character) {
  PutGlyphLocked(std::string_view(&character, 1));
}

void TerminalSession::PutGlyphLocked(std::string_view glyph) {
  // Fast path: a single ASCII byte is always one column, so skip UTF-8 decoding
  // and the width tables entirely for the overwhelmingly common case.
  const bool ascii = glyph.size() == 1 && static_cast<unsigned char>(glyph.front()) < 0x80;
  std::string_view effective_glyph = glyph;
  int width;
  if (ascii) {
    width = 1;
  } else {
    const char32_t codepoint = util::DecodeUtf8Codepoint(glyph);
    // A multibyte sequence that decodes to U+FFFD is malformed (overlong form,
    // surrogate, or out-of-range). Store the replacement character instead of the
    // raw invalid bytes so the cell always holds valid UTF-8 (a genuine U+FFFD
    // encodes to those same bytes, so this substitution is a no-op for it).
    if (codepoint == 0xFFFD) {
      effective_glyph = util::kUtf8ReplacementChar;
    }
    width = util::CodepointDisplayWidth(codepoint);
  }

  // Zero-width (combining marks, variation selectors, joiners): attach to the
  // previously written cell instead of consuming a column, so accents and emoji
  // modifiers stay with their base glyph. Dropped if it would overflow the
  // 4-byte inline cell or there is no base cell yet.
  if (width == 0) {
    if (cursor_column_ == 0) {
      return;
    }
    EnsureCursorLineExistsLocked();
    auto& line = lines_[cursor_row_];
    const std::size_t base = cursor_column_ - 1;
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

  const std::size_t advance = width == 2 ? 2 : 1;

  // Ensure the whole glyph fits on the current row before writing. A
  // double-width glyph cannot straddle the right margin.
  if (columns_ > 0 && cursor_column_ + advance > columns_) {
    if (auto_wrap_mode_) {
      AdvanceCursorRowLocked(true);
    } else {
      cursor_column_ = columns_ - 1;
      // Without autowrap a wide glyph degrades to a single overwrite in place.
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      ResizeLineLocked(line, cursor_column_ + 1);
      BreakWideGlyphPairForWriteLocked(line, cursor_column_, 1);
      line.cells[cursor_column_] = MakeUtf8TerminalCell(effective_glyph, current_style_);
      return;
    }
  }

  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  ResizeLineLocked(line, cursor_column_ + advance);
  BreakWideGlyphPairForWriteLocked(line, cursor_column_, advance);
  line.cells[cursor_column_] = MakeUtf8TerminalCell(effective_glyph, current_style_);
  if (advance == 2) {
    // Trailing spacer carries the lead's style (so background fills span both
    // columns) plus the wide-trailing marker so the renderer skips painting it.
    TerminalCell spacer;
    spacer.style = current_style_;
    spacer.style.set(cell_attr::kWideTrailing, true);
    line.cells[cursor_column_ + 1] = spacer;
  }
  cursor_column_ += advance;
}

void TerminalSession::ResizeLineLocked(TerminalLine& line, std::size_t size) {
  if (line.cells.size() < size) {
    line.cells.resize(size, MakeAsciiTerminalCell(' ', TerminalStyle{}));
  }
}

void TerminalSession::BreakWideGlyphPairForWriteLocked(TerminalLine& line, std::size_t start,
                                                       std::size_t advance) {
  if (advance == 0 || start >= line.cells.size()) {
    return;
  }
  // Left edge: overwriting a trailing spacer orphans its lead one column back.
  if (line.cells[start].style.wide_trailing() && start > 0) {
    line.cells[start - 1] = MakeAsciiTerminalCell(' ', current_style_);
  }
  // Right edge: overwriting a wide lead orphans its trailing spacer one column on.
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
  for (std::size_t i = start; i < end; ++i) {
    line.cells[i] = MakeAsciiTerminalCell(' ', current_style_);
  }
}

void TerminalSession::EraseInLineLocked(int mode) {
  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  switch (mode) {
    case 1: {
      // Clamp the erase end to the right margin: at the pending-wrap column
      // (cursor_column_ == columns_) a bare cursor_column_ + 1 would grow the row
      // one cell past the terminal width, mirroring the EL 2 fix below.
      const std::size_t end =
          columns_ > 0 ? std::min<std::size_t>(cursor_column_ + 1, columns_) : cursor_column_ + 1;
      ClearLineRangeLocked(line, 0, end);
      break;
    }
    case 2: {
      // Erase the entire line: span the full terminal width, not just up to the
      // cursor. Using cursor_column_ + 1 left a line shorter than the cursor only
      // partly cleared and, at the pending-wrap column (cursor_column_ ==
      // columns_), grew the line one cell past the right margin.
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
  // Background Color Erase: an erase with a non-default background (an explicit
  // background color, or reverse video which swaps in the foreground) must paint
  // the blanked cells with that background. A default-background erase renders the
  // same whether the row is empty or full of default-styled blanks, so keep the
  // cheap empty reset for it to avoid materializing a full-width row per clear.
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
      // Erase from the start of the *visible screen* to the cursor. On the primary
      // buffer that start is the top of the viewport, not the top of scrollback —
      // blanking absolute row 0 would destroy history above the screen.
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
        // ED 2 erases the visible screen in place and MUST preserve scrollback
        // (this is what `clear`/`tput clear`'s `ESC[2J` sends). Blank only the last
        // `rows_` lines of the deque; everything above is history. Previously this
        // collapsed the whole deque, silently destroying all scrollback.
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
      // ED 3 (xterm "Erase Saved Lines"): drop the scrollback, leave the visible
      // screen intact. Previously mode 3 fell through to the ED-0 path below,
      // which erases from the cursor to the end of the display — so a bare
      // `\x1b[3J` (not preceded by 2J) destroyed on-screen content, the opposite
      // of the spec. On the primary screen the visible viewport is the last
      // `rows_` lines of the deque; everything before that is scrollback. Trim it
      // and shift the cursor bookkeeping down, mirroring TrimScrollbackLocked. The
      // alternate screen has no scrollback, so 3J is a no-op there.
      if (!use_alternate_screen_) {
        const std::size_t visible = std::max<std::size_t>(1, rows_);
        if (lines_.size() > visible) {
          const std::size_t trim_count = lines_.size() - visible;
          lines_.erase(lines_.begin(),
                       lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
          cursor_row_ = cursor_row_ > trim_count ? cursor_row_ - trim_count : 0;
          saved_cursor_row_ = saved_cursor_row_ > trim_count ? saved_cursor_row_ - trim_count : 0;
          // Account the front-trim exactly as TrimScrollbackLocked does. Workspace
          // scroll/selection mirrors rebase against the *delta* of ScrollbackTrimTotal();
          // omitting it here would strand those absolute rows `trim_count` too high after
          // a `clear`/`tput clear` (which emits ED2 then ED3).
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

void TerminalSession::ResetTabStopsLocked() {
  const std::size_t width = std::max<std::size_t>(1, columns_);
  tab_stops_.assign(width, false);
  for (std::size_t column = 8; column < width; column += 8) {
    tab_stops_[column] = true;
  }
}

void TerminalSession::ResizeTabStopsLocked() {
  // A resize must preserve custom HTS/TBC tab stops in the surviving columns
  // (xterm behaviour); only newly-exposed columns get the default every-8 stops.
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
  // At or past the last usable column there is no forward tab stop. Return the
  // column unchanged rather than snapping to width-1: at the pending-wrap column
  // (column == columns_) the old code moved the cursor *backward* by one.
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

void TerminalSession::ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count) {
  if (rows_ == 0) {
    return;
  }

  // `top`/`bottom` are screen-relative. On the primary buffer the visible screen
  // begins at PrimaryScreenTopLocked() within the scrollback deque, so offset the
  // rotate range by that base; on the alt screen the base is 0.
  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  const std::size_t screen_top = use_alternate_screen_ ? 0 : PrimaryScreenTopLocked();
  if (lines_.size() < screen_top + terminal_rows) {
    lines_.resize(screen_top + terminal_rows);
  }

  const std::size_t rel_top = std::min(top, terminal_rows - 1);
  const std::size_t rel_bottom = std::clamp(bottom, rel_top, terminal_rows - 1);
  const std::size_t clamped_top = rel_top + screen_top;
  const std::size_t clamped_bottom = rel_bottom + screen_top;
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, begin + static_cast<std::ptrdiff_t>(shift), end);
  // Background Color Erase: lines exposed by the scroll take the current
  // background, matching xterm/VTE (and the ED/EL erase paths above).
  for (auto it = end - static_cast<std::ptrdiff_t>(shift); it != end; ++it) {
    BlankLineToCurrentBackgroundLocked(*it);
  }
}

void TerminalSession::ScrollRegionDownLocked(std::size_t top,
                                             std::size_t bottom,
                                             std::size_t count) {
  if (rows_ == 0) {
    return;
  }

  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  const std::size_t screen_top = use_alternate_screen_ ? 0 : PrimaryScreenTopLocked();
  if (lines_.size() < screen_top + terminal_rows) {
    lines_.resize(screen_top + terminal_rows);
  }

  const std::size_t rel_top = std::min(top, terminal_rows - 1);
  const std::size_t rel_bottom = std::clamp(bottom, rel_top, terminal_rows - 1);
  const std::size_t clamped_top = rel_top + screen_top;
  const std::size_t clamped_bottom = rel_bottom + screen_top;
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, end - static_cast<std::ptrdiff_t>(shift), end);
  // Background Color Erase: lines exposed by the scroll take the current
  // background, matching xterm/VTE (and the ED/EL erase paths above).
  for (auto it = begin; it != begin + static_cast<std::ptrdiff_t>(shift); ++it) {
    BlankLineToCurrentBackgroundLocked(*it);
  }
}

void TerminalSession::TrimScrollbackLocked() {
  const std::size_t max_lines = use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                                      : max_scrollback_lines_ +
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
  scrollback_trim_total_ += static_cast<std::uint64_t>(trim_count);
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
}
}  // namespace microide::terminal
