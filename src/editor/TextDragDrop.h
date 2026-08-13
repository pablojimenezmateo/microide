#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "editor/EditTypes.h"

namespace microide::editor {

class TextViewport;

// Dragging a selection to move it (VS Code's `editor.dragAndDrop`, on by
// default): press inside an existing selection, drag, and the selected text
// MOVES to the drop point — with Ctrl held, it copies.
//
// The gesture's plumbing lives in the mouse coordinator; the EDIT lives here,
// because it is the part with arithmetic worth testing on its own. Two things
// make it more than "delete then insert":
//
//  - the delete SHIFTS the drop point when the drop is after the source, and by
//    a different amount depending on whether it shares the source's last line;
//  - it has to be ONE undo entry, or Ctrl+Z leaves the document with the text in
//    neither place.
namespace text_drag_drop {

// Lexicographic (line, column) order.
bool PositionBefore(const TextPosition& lhs, const TextPosition& rhs);

// True when `position` lies within `range` (endpoints included). A drop there is
// a no-op: the text would be moved onto itself, and running the edit anyway
// would delete it and reinsert it at a point that no longer exists.
bool PositionWithin(const SelectionRange& range, const TextPosition& position);

// Where `drop` ends up once `source` has been removed from the document.
// Unchanged when the drop is before the source.
TextPosition AdjustDropForRemovedRange(const SelectionRange& source, const TextPosition& drop);

// The position `start` advances to after `text` is inserted there.
TextPosition PositionAfterInsertedText(const TextPosition& start, std::string_view text);

// Move (or, with `copy`, duplicate) `source` to `drop` as ONE undo entry.
// Returns the range the text occupies afterward — callers select it, as VS Code
// does — or nullopt when the drop is inside the source, which is a no-op rather
// than a destructive edit.
std::optional<SelectionRange> Apply(TextViewport& viewport,
                                    const SelectionRange& source,
                                    const TextPosition& drop,
                                    bool copy);

}  // namespace text_drag_drop

}  // namespace microide::editor
