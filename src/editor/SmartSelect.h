#pragma once

// Expand Selection without a language server (VS Code's smartSelect.expand,
// Shift+Alt+Right, falls back to the same ladder when no provider answers):
// the word under the caret, then a string's content and the whole string, then
// each enclosing bracket pair's content and the pair itself (across lines),
// then the line's text, the whole line, and the document. Each step is the
// smallest of those ranges that strictly contains the current selection.
//
// Brackets inside string literals on the scanned lines are skipped; comments are
// not modelled, which is the price of not asking the highlighter.

#include <optional>

#include "editor/EditTypes.h"
#include "editor/TextBuffer.h"

namespace microide::editor {

// The next larger range, or nullopt when the selection already covers the
// document. `current` is ordered (start <= end) and may be empty.
std::optional<SelectionRange> ExpandSelectionRange(const TextBuffer& lines, SelectionRange current);

}  // namespace microide::editor
