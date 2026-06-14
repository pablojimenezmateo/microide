#pragma once

#include <cstddef>
#include <utility>
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
  T moved = std::move(vec[active]);
  vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(active));
  vec.insert(vec.begin() + static_cast<std::ptrdiff_t>(target), std::move(moved));
  active = target;
  return true;
}

}  // namespace microide::workspace
