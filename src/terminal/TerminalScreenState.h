#pragma once

#include <cstddef>
#include <deque>
#include <string_view>

#include "terminal/TerminalCell.h"

namespace microide::terminal {

struct TerminalCharsets {
  bool g0_dec_graphics = false;
  bool g1_dec_graphics = false;
  bool shifted_out = false;

  bool DecGraphicsActive() const { return shifted_out ? g1_dec_graphics : g0_dec_graphics; }
  friend bool operator==(const TerminalCharsets&, const TerminalCharsets&) = default;
};

std::string_view DecSpecialGraphicsGlyph(unsigned char byte);

struct TerminalScreenState {
  std::deque<TerminalLine> lines = {TerminalLine{}};
  std::size_t cursor_row = 0;
  std::size_t cursor_column = 0;
  std::size_t saved_cursor_row = 0;
  std::size_t saved_cursor_column = 0;
  TerminalStyle saved_style = {};
  TerminalColor saved_underline_color;
  bool saved_origin_mode = false;
  TerminalCharsets saved_charsets = {};
  std::size_t scroll_region_top = 0;
  std::size_t scroll_region_bottom = 0;
};

}
