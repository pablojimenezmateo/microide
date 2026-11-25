#include "terminal/TerminalHoverAffordance.h"

#include <algorithm>

namespace microide::terminal {

namespace {

bool SameText(const TerminalCell& a, const TerminalCell& b) {
  return a.DisplayText() == b.DisplayText();
}

bool SameStyle(const TerminalCell& a, const TerminalCell& b) {
  return a.style.attrs == b.style.attrs && a.style.foreground == b.style.foreground &&
         a.style.background == b.style.background;
}

}

bool TerminalHoverAffordance::Lit(std::size_t column, const TerminalLine& line) const {
  if (column >= before_.size() || column >= line.cells.size()) {
    return false;
  }
  const TerminalCell& was = before_[column];
  const TerminalCell& now = line.cells[column];
  return SameText(was, now) && !SameStyle(was, now);
}

void TerminalHoverAffordance::Probe(std::size_t row, std::size_t column, const TerminalLine& line) {
  if (armed_ && row == row_ && Contains(row, column, line)) {
    return;
  }
  armed_ = true;
  row_ = row;
  column_ = column;
  before_.assign(line.cells.begin(), line.cells.end());
}

bool TerminalHoverAffordance::Contains(std::size_t row, std::size_t column,
                                       const TerminalLine& line) const {
  if (!armed_ || row != row_ || !Lit(column_, line)) {
    return false;
  }
  const std::size_t from = std::min(column, column_);
  const std::size_t to = std::max(column, column_);
  for (std::size_t c = from; c <= to; ++c) {
    if (!Lit(c, line)) {
      return false;
    }
  }
  return true;
}

void TerminalHoverAffordance::Reset() {
  armed_ = false;
  before_.clear();
}

}
