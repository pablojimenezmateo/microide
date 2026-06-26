#include "editor/EditorRowYLayout.h"

namespace microide::editor {

EditorRowYLayout::HitResult EditorRowYLayout::HitTest(float y, std::size_t visible_rows) const {
  if (visible_rows == 0) {
    return HitResult{0, false};
  }
  for (std::size_t row = 0; row < visible_rows; ++row) {
    const float top = RowTop(row);
    const float above = GapAbove(row);
    if (y < top - above) {
      // Above this row's inset strip (and its text) -> clamp up to the row.
      return HitResult{row, false};
    }
    if (above > 0.0f && y < top) {
      // Inside the inert above-line code-lens strip.
      return HitResult{row, true};
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

}  // namespace microide::editor
