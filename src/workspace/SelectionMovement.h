#pragma once

#include <algorithm>
#include <cstddef>

namespace microide::workspace {

// Clamp `*selected_index + delta` into [0, item_count - 1]. Returns true when
// the call had an effect (non-empty range and non-zero delta).
inline bool MoveSelectionIndex(std::size_t item_count, std::size_t* selected_index, int delta) {
  if (item_count == 0 || selected_index == nullptr || delta == 0) {
    return false;
  }
  const int current = static_cast<int>(*selected_index);
  const int max_index = static_cast<int>(item_count) - 1;
  *selected_index = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  return true;
}

template <typename Range>
bool MoveSelectionIndex(const Range& range, std::size_t* selected_index, int delta) {
  return MoveSelectionIndex(range.size(), selected_index, delta);
}

}  // namespace microide::workspace
