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
// VS Code's copyLinesDownAction / copyLinesUpAction. Whole LINES are copied --
// a partial selection duplicates the lines it touches, not the selected text --
// and `downward` decides only which of the two copies the caret and selection
// end up on. Applies to every caret region.
bool CopyLines(TextViewport& viewport, bool downward);

// VS Code's insertLineAfter (Ctrl+Enter) / insertLineBefore (Ctrl+Shift+Enter):
// open a new line below/above the caret's line and put the caret on it,
// regardless of where in the line the caret sat. `Below` runs the language's
// smart indent (so it opens a body after a `{`); `Above` takes the pushed-down
// line's own indent.
bool InsertLineBelow(TextViewport& viewport);
bool InsertLineAbove(TextViewport& viewport);
bool DeleteLine(TextViewport& viewport);
bool IndentSelection(TextViewport& viewport);
bool OutdentSelection(TextViewport& viewport);

// What the Tab key does, decided once for the whole caret set (VS Code's
// TypeOperations.tab): Shift outdents the caret regions; a selection at ANY
// caret that spans lines, or covers a whole line's content, indents the caret
// regions as a block; otherwise every caret inserts one indent, replacing its
// single-line selection if it has one. Both editable key surfaces (the editor
// pane and the compare/merge panes) route through this so they cannot drift --
// each used to hand-copy the test, read the primary only, and replace a
// whole-line selection with four spaces where VS Code indents the line.
enum class TabKeyIntent { kInsertTab, kIndentBlock, kOutdent };
[[nodiscard]] TabKeyIntent ClassifyTabKey(const TextViewport& viewport, bool shift_held);

// VS Code's editor.action.joinLines. NOT named JoinLines: `util::JoinLines`
// already joins a span of strings, and a second overload in this namespace is
// found by ADL from any `TextViewport` argument -- it silently outranked a test's
// own `JoinLines(viewport)` helper the first time this was written.
//
// Folds each caret's region onto one line,
// separated by a single space with the appended lines' leading whitespace
// trimmed. A bare caret joins its line with the one BELOW it (which is what makes
// repeated presses pull a block up); a selection joins every line it touches.
// Applies to every caret.
bool JoinLinesAtCarets(TextViewport& viewport);

bool SortLines(TextViewport& viewport, bool ascending);

}  // namespace microide::editor
