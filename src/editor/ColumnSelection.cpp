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
      state.cursor.line = state.cursor.line == 0 ? 0 : state.cursor.line - 1;
      break;
    case ColumnSelectDirection::Down:
      state.cursor.line = std::min(state.cursor.line + 1, last_line);
      break;
    case ColumnSelectDirection::Left:
      state.cursor.column = state.cursor.column == 0 ? 0 : state.cursor.column - 1;
      break;
    case ColumnSelectDirection::Right:
      if (state.cursor.column < max_column) {
        ++state.cursor.column;
      }
      break;
  }

  state.cursor.line = std::min(state.cursor.line, last_line);
  state.anchor.line = std::min(state.anchor.line, last_line);
  return state;
}

}
