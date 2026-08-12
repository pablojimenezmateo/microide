#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include "util/StringUtil.h"

namespace microide::editor {

// Identifier byte for word-boundary scanning: ASCII alphanumeric plus
// underscore. Matches the convention used by selection-by-word and
// occurrence-highlight seeding.
inline bool IsIdentifierByte(char c) {
  return util::IsAsciiAlnum(static_cast<unsigned char>(c)) || c == '_';
}


struct TextPosition {
  std::size_t line = 0;
  std::size_t column = 0;
};

using LogicalPosition = TextPosition;

// Which soft-wrapped row owns a caret that sits exactly on a wrap boundary.
//
// Wrapped rows are contiguous in visual columns: row N ends where row N+1
// begins, so ONE text position -- the wrap point -- is addressable as both "one
// past the end of row N" and "the start of row N+1". Without a tiebreaker the
// row that owns it is always the next one, which makes vertical motion onto a
// short row bounce straight back down (Up appeared to do nothing at all) and
// makes a click past a wrapped row's last glyph land on the row below. VS Code
// models the same ambiguity as PositionAffinity; this is that bit.
//
// kNextRow is the default and the only value a caret ever *keeps*: it is set by
// vertical motion and hit-testing when they deliberately land on a boundary, and
// every other caret placement resets it.
enum class WrapRowAffinity : std::uint8_t {
  kNextRow = 0,
  kPreviousRow = 1,
};

inline bool operator==(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line == rhs.line && lhs.column == rhs.column;
}

struct SelectionRange {
  TextPosition start;
  TextPosition end;
};

struct AppliedEdit {
  SelectionRange range_before;
  std::string replacement_text;
};

// Whole-line-trimmed span of the last applied edit, in the same coordinate model
// as WorkspaceShell::ComputeChangedLineSpan (common leading/trailing IDENTICAL
// lines trimmed): before-lines occupied [old_start, old_end), after-lines occupy
// [old_start, new_end). Lets edit-side-effect consumers (e.g. merge conflict
// tracking) shift/invalidate line-keyed state from a single edit without diffing a
// whole-buffer before/after snapshot. Empty for true multi-region edits (the
// consumer takes its resync fallback), matching AppliedEdit's contract.
struct AppliedEditLineSpan {
  std::size_t old_start = 0;
  std::size_t old_end = 0;
  std::size_t new_end = 0;
};

}  // namespace microide::editor
