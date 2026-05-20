#pragma once

#include <cstddef>
#include <string>

namespace microide::editor {

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
