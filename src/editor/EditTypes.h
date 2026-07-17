#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace microide::editor {

// Identifier byte for word-boundary scanning: ASCII alphanumeric plus
// underscore. Matches the convention used by selection-by-word and
// occurrence-highlight seeding.
inline bool IsIdentifierByte(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}


struct TextPosition {
  std::size_t line = 0;
  std::size_t column = 0;
};

using LogicalPosition = TextPosition;

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
