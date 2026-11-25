#pragma once

#include <cstddef>
#include <cstdint>

#include "editor/EditTypes.h"

namespace microide::editor {

enum class ColumnSelectDirection {
  Up,
  Down,
  Left,
  Right,
};

struct ColumnSelectionState {
  bool active = false;
  TextPosition anchor;
  TextPosition cursor;
  std::uint64_t document_revision = 0;
};

ColumnSelectionState StepColumnSelection(ColumnSelectionState state,
                                         ColumnSelectDirection direction,
                                         TextPosition caret,
                                         std::size_t line_count,
                                         std::size_t max_column);

}
