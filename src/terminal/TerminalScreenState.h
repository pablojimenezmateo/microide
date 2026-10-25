#pragma once

#include <cstddef>
#include <deque>

#include "terminal/TerminalCell.h"

namespace microide::terminal {

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
  std::size_t scroll_region_top = 0;
  std::size_t scroll_region_bottom = 0;
};

}  // namespace microide::terminal
