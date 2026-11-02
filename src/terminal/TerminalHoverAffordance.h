#pragma once

#include <cstddef>
#include <vector>

#include "terminal/TerminalCell.h"

namespace microide::terminal {

// Which cells a program that owns the mouse treats as clickable, inferred from
// the only thing it tells us: how it repaints under a resting pointer.
//
// A TUI in any-motion tracking (DECSET 1003) is sent every pointer move, and a
// program that has a control under the pointer answers by restyling it -- Claude
// Code's "Jump to bottom" pill lightens, a collapsed "Listed 1 directory" group
// brightens -- and restores the style when the pointer leaves. It never names a
// pointer shape (no OSC 22), so without this the panel can only show the arrow
// over every one of its buttons. The rule is narrow on purpose: the cell under
// the pointer must keep its TEXT and change its STYLE after the move was
// reported. A program redrawing for any other reason changes text (a scroll, a
// spinner) or does not touch that cell; a program that ignores hover changes
// nothing; both leave the arrow.
//
// The run that lights up is the hover target: every contiguous cell on the
// probed row whose text matches the snapshot and whose style does not. Moving
// within it keeps the snapshot (the program has already restyled the cells the
// pointer is moving onto); moving out of it takes a new one.
//
// Pure and lock-free: the session calls it under its own lock, on its own
// screen rows, on the input path (Probe) and the cursor-resolve path
// (Contains). Neither touches the output path, so a stream of output costs
// nothing here.
class TerminalHoverAffordance {
 public:
  // The pointer was reported to the program at screen (row, column); `line` is
  // that screen row as it is now. A move inside the lit run keeps the snapshot.
  void Probe(std::size_t row, std::size_t column, const TerminalLine& line);
  // Whether the cell at screen (row, column) is part of the lit run, judged on
  // `line`, that screen row as it is now.
  bool Contains(std::size_t row, std::size_t column, const TerminalLine& line) const;
  // The program left mouse tracking, the screen was reset or switched, or the
  // session restarted: nothing reported earlier describes the screen any more.
  void Reset();

 private:
  // A cell the program restyled since the snapshot, text unchanged.
  bool Lit(std::size_t column, const TerminalLine& line) const;

  bool armed_ = false;
  std::size_t row_ = 0;
  std::size_t column_ = 0;
  // The probed row as it looked before the program saw the pointer; its buffer
  // is reused across probes.
  std::vector<TerminalCell> before_;
};

}  // namespace microide::terminal
