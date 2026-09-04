#pragma once

#include <cstddef>

namespace microide::terminal {

inline constexpr std::size_t kMaxScrollbackLines = 2000;


// The pending-wrap (LCF) column: after a glyph lands in the last column the
// cursor is kept at `column == columns` so the NEXT glyph wraps, but to every
// other operation the cursor is on the last column, as in xterm — CUB 1 from
// there lands on the second-to-last column, EL erases the last glyph, ECH/ICH/
// DCH act on it, a save records it. Each of those was one column off right
// after a line was filled, which is exactly where readline and zsh redraw a
// prompt at the margin (found against pyte). Any such operation clears the
// pending wrap, exactly as cursor motion clears xterm's wrap flag.
inline void ClampPendingWrapColumn(std::size_t& column, std::size_t columns) {
  if (columns > 0 && column >= columns) {
    column = columns - 1;
  }
}

}  // namespace microide::terminal
