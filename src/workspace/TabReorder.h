#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace microide::workspace {

// Moves `vec[active]` to position `target`, updating `active` to point at the
// moved element. Returns false on an out-of-range index and true otherwise
// (including the `active == target` no-op, matching the historical
// per-strip move semantics). Pure container operation: callers remain
// responsible for cache invalidation, focus, visibility, and persistence.
template <typename T>
bool ReorderActive(std::vector<T>& vec, std::size_t& active, std::size_t target) {
  if (active >= vec.size() || target >= vec.size()) {
    return false;
  }
  if (active == target) {
    return true;
  }
  // One rotate rather than erase-then-insert: that pair moved every element
  // between the two positions twice, plus a temporary hoisted out and back in,
  // for tab types that carry viewports and document state.
  const auto begin = vec.begin();
  const auto at = [&](std::size_t i) { return begin + static_cast<std::ptrdiff_t>(i); };
  if (active < target) {
    std::rotate(at(active), at(active + 1), at(target + 1));
  } else {
    std::rotate(at(target), at(active), at(active + 1));
  }
  active = target;
  return true;
}

}  // namespace microide::workspace
