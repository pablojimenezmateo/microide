#pragma once

// Which wrapped row paints a caret.
//
// Wrapped rows are contiguous in visual columns, so the wrap point is ONE text
// position that two rows both answer for: the trailing edge of the row that ends
// there and the leading edge of the row that begins there. The primary caret
// settles this with an explicit affinity bit carried through vertical motion
// (WrapRowAffinity, VS Code's PositionAffinity). Secondary carets carry the same
// bit -- and the paint loop did not read it, so it decided with a first-row-only
// heuristic instead:
//
//   * a secondary caret ON a wrap boundary was drawn TWICE, once at the end of
//     the row above and once at the start of the row below;
//   * a secondary caret at the END of a wrapped line was drawn NOT AT ALL, because
//     the heuristic compared an absolute visual column against a per-row cell
//     COUNT and those only coincide on a line's first row.
//
// The rule below is the same one the primary caret follows, stated once so both
// can be tested without a renderer.

#include <cstddef>

#include "editor/EditTypes.h"

namespace microide::editor {

// True when the row spanning visual columns [row_visual_start, row_visual_end)
// is the one that should paint a caret at `caret_visual_column`.
//
// `row_is_line_first` / `row_is_line_last` describe the row's place among the
// wrapped rows of its own logical line (both true for an unwrapped line). The
// end-of-line caret sits AT row_visual_end, which is inside no row's half-open
// span, so the line's last row claims it.
[[nodiscard]] constexpr bool WrappedRowPaintsCaret(std::size_t caret_visual_column,
                                                   std::size_t row_visual_start,
                                                   std::size_t row_visual_end,
                                                   bool row_is_line_first,
                                                   bool row_is_line_last,
                                                   WrapRowAffinity affinity) {
  if (caret_visual_column < row_visual_start || caret_visual_column > row_visual_end) {
    return false;
  }
  if (caret_visual_column == row_visual_end) {
    // The end of the line belongs to the line's last row -- there is no row after
    // it to hand the caret to. Otherwise this is the wrap boundary, and only a
    // caret that has chosen the PREVIOUS row renders at this trailing edge.
    return row_is_line_last || affinity == WrapRowAffinity::kPreviousRow;
  }
  if (caret_visual_column == row_visual_start && !row_is_line_first) {
    // The other side of the same boundary: a caret that chose the previous row was
    // already painted there, so this row must not paint it again.
    return affinity != WrapRowAffinity::kPreviousRow;
  }
  return true;
}

}  // namespace microide::editor
