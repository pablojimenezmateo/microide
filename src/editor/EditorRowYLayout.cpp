#include "editor/EditorRowYLayout.h"

namespace microide::editor {

EditorRowYLayout::HitResult EditorRowYLayout::HitTest(float y, std::size_t visible_rows) const {
  if (visible_rows == 0) {
    return HitResult{0, false};
  }
  for (std::size_t row = 0; row < visible_rows; ++row) {
    const float top = RowTop(row);
    if (y < top) {
      // Above the first row's top -> clamp to the first row.
      return HitResult{row, false};
    }
    if (y < top + line_height_) {
      return HitResult{row, false};
    }
    const float gap = GapHeightBelow(row);
    if (gap > 0.0f && y < top + line_height_ + gap) {
      return HitResult{row, true};
    }
  }
  return HitResult{visible_rows - 1, false};
}

float EditorRowYLayout::WindowHeight(std::size_t visible_rows) const {
  if (visible_rows == 0) {
    return 0.0f;
  }
  // RowTop(visible_rows - 1) already folds in every gap above the last row; add
  // that last row's own height plus any gap directly below it.
  return (RowTop(visible_rows - 1) - RowTop(0)) + line_height_ +
         GapHeightBelow(visible_rows - 1);
}

}  // namespace microide::editor
