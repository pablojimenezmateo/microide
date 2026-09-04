#pragma once

// Keyboard column (box) selection: Ctrl+Shift+Alt+Arrow, as in VSCode.
//
// Mouse box selection already existed (Shift+Alt+drag, see
// WorkspaceEditorMouseCoordinator), and TextViewport::SetBoxSelection already did
// the hard part — per-line column clamping and the 10,000-caret span cap. What was
// missing was the keyboard half: with no pointer there was no way to make a
// rectangular selection at all.
//
// The state machine is separated from the viewport so the arrow-key arithmetic can
// be tested directly. Columns here are VISUAL columns (cells), as in VSCode: one
// step is one cell whatever the bytes underneath, and the box stays a rectangle
// across tabs and multi-byte text. The interesting part is the *virtual* cursor
// column: VSCode lets a column selection extend past the end of a short line, and
// the caret keeps its virtual column so moving down onto a long line restores the
// full width. TextViewport clamps per line when it materializes the carets, so the
// virtual column has to be tracked out here or it is lost on the first short line.

#include <cstddef>

#include "editor/EditTypes.h"

namespace microide::editor {

enum class ColumnSelectDirection {
  Up,
  Down,
  Left,
  Right,
};

struct ColumnSelectionState {
  // False until the first column-select keystroke. Any other caret movement resets
  // it, so a fresh gesture anchors at wherever the caret ended up.
  bool active = false;
  // Fixed corner, captured when the gesture starts. `column` is a visual column.
  TextPosition anchor;
  // Moving corner. Its column is a virtual visual column: it may exceed the width
  // of the line it currently sits on.
  TextPosition cursor;
};

// Advances one step. When `state` is inactive both corners start at `caret`
// (given with its visual column).
//
// `line_count` bounds vertical motion; `max_column` bounds the virtual column so
// Right cannot grow without limit on a document whose lines are all short. Callers
// pass the visual width of the widest line currently spanned by the box, which is
// what a user can actually select.
ColumnSelectionState StepColumnSelection(ColumnSelectionState state,
                                         ColumnSelectDirection direction,
                                         TextPosition caret,
                                         std::size_t line_count,
                                         std::size_t max_column);

}  // namespace microide::editor
