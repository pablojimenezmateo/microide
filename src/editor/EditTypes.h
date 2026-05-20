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

}  // namespace microide::editor
