#pragma once

#include <cstddef>

#include "editor/EditTypes.h"

namespace microide::editor {

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
    return row_is_line_last || affinity == WrapRowAffinity::kPreviousRow;
  }
  if (caret_visual_column == row_visual_start && !row_is_line_first) {
    return affinity != WrapRowAffinity::kPreviousRow;
  }
  return true;
}

}
