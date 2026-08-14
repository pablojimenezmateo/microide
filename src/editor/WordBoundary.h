#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace microide::editor {

// Word-granular boundary scanning over one line of text.
//
// This is the single rule behind every word-wise gesture in the app:
// Ctrl+Left / Ctrl+Right, Ctrl+Shift+Left / Ctrl+Shift+Right, Ctrl+Backspace and
// Ctrl+Delete, in the multi-line editor and in every single-line field. It used
// to exist only as three file-local statics inside `SingleLineEditor.cpp`, so
// the main editor -- the one surface where word motion matters most -- had no
// word verbs at all and Ctrl+Left moved one character.
//
// The classification is VS Code's (`wordOperations.ts`): three classes, and a
// *run* of either non-whitespace class is a word. `foo === bar` therefore stops
// at the end of `foo`, at the end of `===`, and at the end of `bar`, rather than
// treating `=== ` as undifferentiated filler and skipping it.
enum class WordClass : std::uint8_t {
  kWhitespace = 0,
  // Identifier content: ASCII `[A-Za-z0-9_]` plus non-ASCII letters, so `café`
  // and `ñoño` are one word each rather than one stop per byte.
  kWord,
  // Everything else that is not whitespace: punctuation, operators, brackets.
  kSeparator,
};

// Class of the code point beginning at byte `index`. Decodes the whole scalar,
// so a multi-byte letter classifies as kWord rather than as three separators.
// `index` is expected to be a UTF-8 boundary; out-of-range yields kWhitespace.
[[nodiscard]] WordClass ClassifyWordCodepointAt(std::string_view text, std::size_t index);

// Previous word edge (VS Code `cursorWordStartLeft`): skip whitespace left of
// `caret`, then walk to the start of the run of same-class code points that ends
// there. Returns 0 when `caret` is already at or before the first word.
[[nodiscard]] std::size_t WordBoundaryLeft(std::string_view text, std::size_t caret);

// Next word edge (VS Code `cursorWordEndRight`): skip whitespace at `caret`,
// then walk to the end of the run of same-class code points that begins there.
// Returns `text.size()` when nothing but whitespace remains.
[[nodiscard]] std::size_t WordBoundaryRight(std::string_view text, std::size_t caret);

// Start of the span Ctrl+Backspace should remove from `caret`, i.e. VS Code's
// `deleteWordLeft` with `whitespaceHeuristics` on: a run of **two or more**
// whitespace code points immediately left of the caret is removed on its own,
// leaving the word before it alone. That is what makes backspacing out of an
// indent land on column 0 instead of eating the previous line's last word.
// Falls back to `WordBoundaryLeft` otherwise.
[[nodiscard]] std::size_t DeleteWordBoundaryLeft(std::string_view text, std::size_t caret);

// End of the span Ctrl+Delete should remove from `caret`. Mirror of the above:
// a run of two or more whitespace code points at the caret goes on its own.
[[nodiscard]] std::size_t DeleteWordBoundaryRight(std::string_view text, std::size_t caret);

struct WordSpan {
  std::size_t start = 0;
  std::size_t end = 0;

  [[nodiscard]] bool empty() const { return start >= end; }
};

// The maximal run of identifier code points covering byte `index`, or an empty
// span when the code point there is not identifier content. `index` is snapped
// back to a UTF-8 boundary first, so a caret placed mid-scalar by a mouse hit or
// a plugin cannot split a character.
//
// This is what double-click-to-select-a-word and occurrence highlighting stand
// on. Both used to scan with a byte-wise ASCII predicate, so double-clicking
// `café` selected `caf` and highlighted the wrong occurrences with it.
[[nodiscard]] WordSpan IdentifierRunAt(std::string_view text, std::size_t index);

}  // namespace microide::editor
