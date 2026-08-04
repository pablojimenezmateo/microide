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
      // work), but not past the longest line the box covers.
      state.cursor.column = std::min(state.cursor.column + 1, max_column);
      break;
  }

  state.cursor.line = std::min(state.cursor.line, last_line);
  state.anchor.line = std::min(state.anchor.line, last_line);
  return state;
}

}  // namespace microide::editor
