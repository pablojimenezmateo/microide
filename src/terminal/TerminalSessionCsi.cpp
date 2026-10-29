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

// CPR: the answer to `CSI 6 n` (or `CSI ? 6 n` when `private_form`), the cursor's
// 1-based screen row and column.
std::string CursorPositionReport(bool private_form, std::size_t cursor_row,
                                 std::size_t cursor_column, std::size_t origin_row,
                                 std::size_t columns) {
  // At the pending-wrap (LCF) column cursor_column == columns; a real terminal
  // reports the last on-screen column, never one past the edge.
  const std::size_t reported_column = columns > 0 ? std::min(cursor_column, columns - 1)
                                                  : cursor_column;
  // Report the row relative to the visible screen (the scroll region under
  // DECOM, as xterm does): on the primary buffer the cursor row is an absolute
  // deque index that includes scrollback, so a raw cursor_row+1 would grossly
  // overstate the row.
  const std::size_t reported_row = cursor_row > origin_row ? cursor_row - origin_row : 0;
  return (private_form ? "\x1b[?" : "\x1b[") + std::to_string(reported_row + 1) + ";" +
         std::to_string(reported_column + 1) + "R";
}

// Where IL/DL act: the cursor row and the scroll region's bottom, as
// ScrollRegionUp/DownLocked take them (relative to `screen_top`).
struct ScrollSpan {
  std::size_t top = 0;
  std::size_t bottom = 0;
};

// IL and DL are bounded by the scroll region (or the visible screen when no
// custom region is set), NOT the whole scrollback deque: inserting into the
// absolute deque and trimming the front shifted the scrollback / visible
// boundary. Outside the margins both are no-ops (DEC STD 070); on the
// fixed-height alt grid, acting anyway would grow the deque past rows_ and
// eventually trim real content off the top. `screen_top` is 0 on the alt
// screen, so one computation serves both buffers.
std::optional<ScrollSpan> CursorScrollSpan(std::size_t cursor_row, std::size_t screen_top,
                                           std::size_t region_top, std::size_t region_bottom) {
  if (cursor_row < screen_top + region_top || cursor_row > screen_top + region_bottom) {
    return std::nullopt;
  }
  return ScrollSpan{.top = cursor_row - screen_top, .bottom = region_bottom};
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
  if (!body.empty() && !util::IsAsciiDigit(static_cast<unsigned char>(body.front())) &&
      body.front() != ';') {
    prefix = body.front();
    body.remove_prefix(1);
  }
  // SGR (`CSI ... m` with no prefix/intermediate) re-parses the body itself via
  // ApplySgrParameters, so the general parameter vector would be built and
  // discarded. Short-circuit the most common colored-output sequence before it.
  if (prefix == '\0' && intermediate == '\0' && final == 'm') {
    detail::ApplySgrParameters(current_style_, body);
    return;
  }
  // Reuse a thread_local buffer so a non-SGR CSI does not heap-allocate a fresh
  // params vector each call (the SGR fast path above already reuses a thread_local
  // groups buffer). HandleEscapeSequenceLocked is not re-entered while `params` is
  // live, so a single shared scratch is safe.
  thread_local std::vector<int> params_scratch;
  ParseCsiParametersInto(body, params_scratch);
  std::vector<int>& params = params_scratch;

  // The deque row that row-addressing CSIs (CUP, VPA) count from and that
  // CUU/CPL stop at: the visible screen's top (0 on the alternate screen), or the
  // scroll region's top under origin mode (DECOM) -- on either screen, as in xterm.
  const auto addressing_origin_row = [this]() -> std::size_t {
    return PrimaryScreenTopLocked() + (origin_mode_ ? ActiveScrollRegionTopLocked() : 0);
  };
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
        if (mode == 996) {
          // Colour-scheme query (the mode-2031 protocol): 1 dark, 2 light.
          SendBytesLocked(util::IsDarkColor(default_background_) ? "\x1b[?997;1n" : "\x1b[?997;2n");
          continue;
        }
        if (mode == 6) {
          SendBytesLocked(CursorPositionReport(/*private_form=*/true, cursor_row_, cursor_column_,
                                               addressing_origin_row(), columns_));
        }
      }
    }
    return;
  }

  if (prefix == '>') {
    if (final == 'c') {
      SendBytesLocked("\x1b[>0;10;1c");
    } else if (final == 'q' && intermediate == '\0' && (params.empty() || params.front() == 0)) {
      // XTVERSION: name and version, so a program can tell which terminal it is
      // in rather than guess from TERM (which says xterm-256color).
      SendBytesLocked("\x1bP>|microide " MICROIDE_VERSION "\x1b\\");
    }
    return;
  }

  // The pending-wrap (LCF) column: after a glyph lands in the last column the
  // cursor is kept at cursor_column_ == columns_ so the NEXT glyph wraps, but to
  // every other operation the cursor is on the last column, as in xterm — CUB 1
  // from there lands on the second-to-last column, EL erases the last glyph,
  // ECH/ICH/DCH act on it, and a save records it. Without this clamp each of
  // those was one column off right after a line was filled, which is exactly
  // where readline and zsh redraw a prompt at the margin. SGR, DA, DECSCUSR,
  // TBC and DECSTBM leave the cursor alone (an attribute change between the
  // last glyph and the next must not stop the wrap); found against pyte.
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
  const auto cursor_scroll_span = [this]() {
    return CursorScrollSpan(cursor_row_, PrimaryScreenTopLocked(), ActiveScrollRegionTopLocked(),
                            ActiveScrollRegionBottomLocked());
  };
  switch (final) {
    case 'c':
      SendBytesLocked("\x1b[?1;2c");
      return;
    case 'm':
      detail::ApplySgrParameters(current_style_, body);
      return;
    case 'A': {
      // CUU: cursor up, clamped to the TOP OF THE VISIBLE SCREEN, not the top of
      // the deque. On the primary buffer `cursor_row_` is an absolute index into
      // scrollback, so flooring at 0 (as before) let `ESC[500A` ("go to top of
      // screen") climb above the visible rows into history and overwrite it. The
      // floor mirrors the origin CUP (`CSI H`) uses.
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      const std::size_t floor_row = addressing_origin_row();
      cursor_row_ = cursor_row_ >= floor_row + count ? cursor_row_ - count : floor_row;
      return;
    }
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
    case 'D': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      cursor_column_ = cursor_column_ > count ? cursor_column_ - count : 0;
      return;
    }
    case 'E':
      MoveCursorLocked(cursor_row_ + static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                       0);
      return;
    case 'F': {
      // CPL: cursor up N lines to column 0, clamped to the visible-screen top (see
      // CUU above) so it cannot climb into scrollback on the primary buffer.
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      const std::size_t floor_row = addressing_origin_row();
      MoveCursorLocked(cursor_row_ >= floor_row + count ? cursor_row_ - count : floor_row, 0);
      return;
    }
    case 'G':
      MoveCursorLocked(cursor_row_,
                       static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)));
      return;
    case 'H':
    case 'f':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              addressing_origin_row(),
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 1, 1) - 1)));
      return;
    case 'J':
      EraseInDisplayLocked(params.empty() ? 0 : params.front());
      return;
    case 'K':
      EraseInLineLocked(params.empty() ? 0 : params.front());
      return;
    case 'L': {
      std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      // Inserting more blank lines than the screen height shifts the rest off and
      // is trimmed anyway, so clamp: `CSI 65535 L` must not transiently balloon
      // the line deque with 65535 empty lines before TrimScrollbackLocked runs.
      if (rows_ > 0) {
        count = std::min(count, rows_);
      }
      EnsureCursorLineExistsLocked();
      // ScrollRegionDownLocked opens `count` blank lines at the cursor and discards
      // lines pushed past the region bottom.
      if (const auto span = cursor_scroll_span(); span.has_value()) {
        ScrollRegionDownLocked(span->top, span->bottom, count);
      }
      return;
    }
    case 'M': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      // ScrollRegionUpLocked deletes `count` lines at the cursor and pulls blank
      // lines in at the region bottom.
      if (const auto span = cursor_scroll_span(); span.has_value()) {
        ScrollRegionUpLocked(span->top, span->bottom, count);
      }
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
    case 'X': {  // ECH — erase characters.
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      // Clamp the erase range to the terminal width (or the current line length
      // when the width is unset) so a large parameter like `CSI 65535 X` cannot
      // grow the row far past `columns_` — the `@`-insert path clamps the same
      // way. Without this a single escape balloons a row to ~64K cells, and
      // repeated across the scrollback that is a multi-hundred-MB amplification.
      const std::size_t max_end =
          columns_ > 0 ? columns_ : lines_[cursor_row_].cells.size();
      const std::size_t erase_end = std::min(cursor_column_ + count, max_end);
      ClearLineRangeLocked(lines_[cursor_row_], cursor_column_, erase_end);
      return;
    }
    case '@': {
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      // Sized to one past the cursor so the insert always has a row to shift
      // (InsertBlankCellsLocked clamps `CSI 65535 @` to the width).
      ResizeLineLocked(line, cursor_column_ + 1);
      detail::InsertBlankCells(line, cursor_column_,
                               static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)), columns_,
                               current_style_);
      return;
    }
    case 'I': {  // CHT — cursor forward tabulation.
      const std::size_t count =
          std::min<std::size_t>(static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                                std::max<std::size_t>(1, columns_));
      for (std::size_t i = 0; i < count; ++i) {
        cursor_column_ = NextTabStopLocked(cursor_column_);
      }
      return;
    }
    case 'Z': {  // CBT — cursor backward tabulation.
      const std::size_t count =
          std::min<std::size_t>(static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                                std::max<std::size_t>(1, columns_));
      for (std::size_t i = 0; i < count; ++i) {
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
      // SU scrolls the scroll region up. Works on the alt screen, and on the
      // primary buffer when a custom DECSTBM region is active (a full-screen
      // primary SU would discard the top line rather than accumulate scrollback,
      // so it stays a no-op there — the common case is unchanged).
      if (use_alternate_screen_ || HasCustomScrollRegionLocked()) {
        ScrollRegionUpLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                             static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'T':
      if (use_alternate_screen_ || HasCustomScrollRegionLocked()) {
        ScrollRegionDownLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                               static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'n':
      if (!params.empty()) {
        if (params.front() == 5) {
          SendBytesLocked("\x1b[0n");
        } else if (params.front() == 6) {
          SendBytesLocked(CursorPositionReport(/*private_form=*/false, cursor_row_,
                                               cursor_column_, addressing_origin_row(), columns_));
        }
      }
      return;
    case 'd':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              addressing_origin_row(),
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
      // xterm/VTE clamp a bottom margin past the screen height to the last row
      // rather than discarding the whole command (a program that sizes its region
      // to a slightly stale row count still gets a valid region).
      const std::size_t clamped_bottom = std::min(scroll_region_bottom, terminal_rows - 1);
      // Per DEC/VT (DECSTBM) the top margin must be strictly above the bottom;
      // an equal (one-line) or inverted region is invalid and must be ignored.
      if (scroll_region_top >= terminal_rows || scroll_region_top >= clamped_bottom) {
        return;
      }
      scroll_region_top_ = scroll_region_top;
      scroll_region_bottom_ = clamped_bottom;
      MoveCursorLocked(addressing_origin_row(), 0);  // home: the region's top under DECOM
      return;
    }
    // SCOSC / SCORC: xterm treats them as DECSC / DECRC (CASE_ANSI_SC calls
    // CursorSave), rendition and origin mode included.
    case 'b': {  // REP — repeat the preceding graphic character.
      if (last_glyph_length_ == 0) {
        return;
      }
      // A repeat past the whole screen only overwrites what it just wrote: capped
      // there so `CSI 2147483647 b` is not a hang on the reader thread.
      const std::size_t count =
          std::min(static_cast<std::size_t>(std::max(1, CsiParamOrDefault(params, 0, 1))),
                   std::max<std::size_t>(1, rows_) * std::max<std::size_t>(1, columns_));
      // Copied: PutGlyphLocked rewrites `last_glyph_` with the same bytes.
      const std::array<char, 4> glyph = last_glyph_;
      const std::string_view text(glyph.data(), last_glyph_length_);
      for (std::size_t i = 0; i < count; ++i) {
        PutGlyphLocked(text);
      }
      return;
    }
    case 'h':
    case 'l':
      // ANSI (non-private) modes: only IRM (4) has an effect here.
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

}  // namespace microide::terminal
