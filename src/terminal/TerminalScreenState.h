#pragma once

#include <cstddef>
#include <deque>
#include <string_view>

#include "terminal/TerminalCell.h"

namespace microide::terminal {

// The character sets G0 and G1 designate (`ESC ( F` / `ESC ) F`) and which of
// them SO/SI shifted into GL. Only DEC Special Graphics (`F` = '0') maps any
// byte; every other designation is ASCII here. ncurses draws every box with it
// under TERM=xterm-256color, even in a UTF-8 locale (smacs = `ESC ( 0`), so
// without it a border printed as `lqqqqk`. DECSC saves it, as xterm's CursorSave
// does.
struct TerminalCharsets {
  bool g0_dec_graphics = false;
  bool g1_dec_graphics = false;
  bool shifted_out = false;  // SO: G1 is in GL until SI

  bool DecGraphicsActive() const { return shifted_out ? g1_dec_graphics : g0_dec_graphics; }
  friend bool operator==(const TerminalCharsets&, const TerminalCharsets&) = default;
};

// The UTF-8 glyph DEC Special Graphics draws for printable ASCII `byte`, or an
// empty view for a byte it leaves alone (everything outside 0x5F..0x7E).
std::string_view DecSpecialGraphicsGlyph(unsigned char byte);

// What one of a terminal's two screens (primary, alternate) keeps while the
// other is shown: its lines, cursor, saved cursor (DECSC) and scroll region.
// TerminalSession's working copy lives in its own members; switching screens
// saves into and restores from one of these.
struct TerminalScreenState {
  std::deque<TerminalLine> lines = {TerminalLine{}};
  std::size_t cursor_row = 0;
  std::size_t cursor_column = 0;
  std::size_t saved_cursor_row = 0;
  std::size_t saved_cursor_column = 0;
  TerminalStyle saved_style = {};
  bool saved_origin_mode = false;
  TerminalCharsets saved_charsets = {};
  std::size_t scroll_region_top = 0;
  std::size_t scroll_region_bottom = 0;
};

}  // namespace microide::terminal
