#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "util/StringUtil.h"

namespace microide::editor {

inline bool IsIdentifierByte(char c) {
  return util::IsAsciiAlnum(static_cast<unsigned char>(c)) || c == '_';
}


struct TextPosition {
  std::size_t line = 0;
  std::size_t column = 0;
};

using LogicalPosition = TextPosition;

enum class WrapRowAffinity : std::uint8_t {
  kNextRow = 0,
  kPreviousRow = 1,
};

inline bool operator==(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line == rhs.line && lhs.column == rhs.column;
}

inline bool PositionBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
}

struct SelectionRange {
  TextPosition start;
  TextPosition end;
};

inline bool operator==(const SelectionRange& lhs, const SelectionRange& rhs) {
  return lhs.start == rhs.start && lhs.end == rhs.end;
}

inline bool RangeContains(const SelectionRange& outer, const SelectionRange& inner) {
  return !PositionBefore(inner.start, outer.start) && !PositionBefore(outer.end, inner.end);
}

struct AppliedEdit {
  SelectionRange range_before;
  std::string replacement_text;
};

inline TextPosition AppliedEditInsertedEnd(const AppliedEdit& edit) {
  const TextPosition start = PositionBefore(edit.range_before.end, edit.range_before.start)
                                 ? edit.range_before.end
                                 : edit.range_before.start;
  const std::string& text = edit.replacement_text;
  const std::size_t last_break = text.find_last_of('\n');
  if (last_break == std::string::npos) {
    return TextPosition{start.line, start.column + text.size()};
  }
  const auto breaks = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  return TextPosition{start.line + breaks, text.size() - (last_break + 1)};
}

inline TextPosition MapPositionThroughEdit(const TextPosition& position, const AppliedEdit& edit) {
  const bool reversed = PositionBefore(edit.range_before.end, edit.range_before.start);
  const TextPosition before_start = reversed ? edit.range_before.end : edit.range_before.start;
  const TextPosition before_end = reversed ? edit.range_before.start : edit.range_before.end;
  const TextPosition new_end = AppliedEditInsertedEnd(edit);
  if (!PositionBefore(before_start, position)) {
    return position;
  }
  if (PositionBefore(position, before_end)) {
    return new_end;
  }
  if (position.line == before_end.line) {
    return TextPosition{new_end.line, new_end.column + (position.column - before_end.column)};
  }
  return TextPosition{position.line - before_end.line + new_end.line, position.column};
}

struct AppliedEditLineSpan {
  std::size_t old_start = 0;
  std::size_t old_end = 0;
  std::size_t new_end = 0;
};

}
