#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace microide::util {

// Geometric reserve for a REUSED scratch buffer.
//
// `std::vector::reserve(n)` allocates EXACTLY n when n exceeds the current
// capacity — it deliberately does not apply the growth factor that `push_back`
// does, because the caller is assumed to know the final size. That assumption is
// wrong for a scratch buffer that is cleared and refilled with a size which grows
// by a small amount each time: `clear(); reserve(n);` with n rising 1..N
// reallocates and copies on EVERY call, which is exactly the O(N^2) the scratch
// buffer was introduced to avoid. It is a quiet trap, because the code reads as
// the allocation-free version.
//
// Held Ctrl+Shift+Alt+Arrow column selection is the shape that found it: the
// caret set is rebuilt whole per keystroke over a span one line longer than the
// last, so three exact reserves cost three reallocations per keystroke forever.
//
// This grows by max(n, 2 * capacity) instead, so a monotonically growing refill
// pattern reaches a stable capacity in O(log N) allocations and is allocation-free
// after that. A caller that genuinely knows the final size should keep using
// `reserve` — this is for the reuse case only.
template <typename T, typename Alloc>
void ReserveGrowing(std::vector<T, Alloc>& vec, std::size_t required) {
  const std::size_t capacity = vec.capacity();
  if (required <= capacity) {
    return;
  }
  vec.reserve(std::max(required, capacity * 2));
}

}  // namespace microide::util
