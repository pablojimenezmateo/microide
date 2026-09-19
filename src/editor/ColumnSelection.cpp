#include "editor/ColumnSelection.h"

#include <algorithm>

namespace microide::editor {

ColumnSelectionState StepColumnSelection(ColumnSelectionState state,
                                         ColumnSelectDirection direction,
                                         TextPosition caret,
                                         std::size_t line_count,
                                         std::size_t max_column) {
  if (!state.active) {
    state.active = true;
    state.anchor = caret;
    state.cursor = caret;
  }

  const std::size_t last_line = line_count == 0 ? 0 : line_count - 1;
  switch (direction) {
    case ColumnSelectDirection::Up:
      // Saturating, not wrapping: at the top the gesture stays put rather than
      // jumping to the bottom of the file.
      state.cursor.line = state.cursor.line == 0 ? 0 : state.cursor.line - 1;
      break;
    case ColumnSelectDirection::Down:
      state.cursor.line = std::min(state.cursor.line + 1, last_line);
      break;
    case ColumnSelectDirection::Left:
      state.cursor.column = state.cursor.column == 0 ? 0 : state.cursor.column - 1;
      break;
    case ColumnSelectDirection::Right:
      // Past end-of-line is allowed (that is what makes a box over ragged lines
      // work), but the virtual column only GROWS while it is below the widest
      // line the box covers -- VS Code's `columnSelectRight`
      // (`if (toViewVisualColumn < maxVisualViewportColumn) toViewVisualColumn++`).
      //
      // Not `min(column + 1, max_column)`: `max_column` is the width of the span
      // the box covers RIGHT NOW, and that span shrinks when the moving corner
      // walks back toward the anchor. Widen the box over a long line, then step
      // the corner back off it, and the min dragged the virtual column down to
      // the remaining lines' width -- so pressing Right made the selection
      // dramatically NARROWER, and the virtual column the short-line crossing
      // exists to preserve was lost for good.
      if (state.cursor.column < max_column) {
        ++state.cursor.column;
      }
      break;
  }

  state.cursor.line = std::min(state.cursor.line, last_line);
  state.anchor.line = std::min(state.anchor.line, last_line);
  return state;
}

}  // namespace microide::editor
