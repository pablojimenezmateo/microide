#pragma once

#include <string>
#include <string_view>

namespace microide::editor {

class TextViewport;

// Free functions that operate on `TextViewport` to perform line-shaping
// transformations. Each function produces one applied edit (single undo step)
// when the operation is non-trivial; multi-caret variants delegate to the
// viewport's existing multi-caret-aware paths where applicable.

// Toggle line comment using `line_marker` ("//", "#", "--", ...). Empty marker
// is a no-op. Affected line range is the union of all caret lines (primary +
// secondary) and the active selection.
bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker);

// Toggle block comment using `open` / `close`. Empty markers are no-op.
bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close);

bool MoveLineUp(TextViewport& viewport);
bool MoveLineDown(TextViewport& viewport);
bool DuplicateSelection(TextViewport& viewport);
bool DeleteLine(TextViewport& viewport);
bool IndentSelection(TextViewport& viewport);
bool OutdentSelection(TextViewport& viewport);

bool SortLines(TextViewport& viewport, bool ascending);

}  // namespace microide::editor
