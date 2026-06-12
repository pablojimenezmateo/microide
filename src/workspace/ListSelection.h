#pragma once

#include <algorithm>
#include <cstddef>

namespace microide::workspace {

// Standard page jump for keyboard list navigation (PageUp / PageDown) across pickers.
inline constexpr int kListPageStep = 8;

// New selected index after moving `delta` from `current` within `count` items, clamped to
// [0, count - 1]. Returns 0 when the list is empty. For the non-wrapping pickers (file
// finder, project search, completion, code actions, commit picker); buffer search
// intentionally wraps and does not use this.
inline std::size_t ClampListIndexMove(std::size_t current, std::size_t count, int delta) {
  if (count == 0) {
    return 0;
  }
  const long long max_index = static_cast<long long>(count) - 1;
  const long long next = std::clamp(static_cast<long long>(current) + delta, 0LL, max_index);
  return static_cast<std::size_t>(next);
}

}  // namespace microide::workspace
