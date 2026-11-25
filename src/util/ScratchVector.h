#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace microide::util {

template <typename T, typename Alloc>
void ReserveGrowing(std::vector<T, Alloc>& vec, std::size_t required) {
  const std::size_t capacity = vec.capacity();
  if (required <= capacity) {
    return;
  }
  vec.reserve(std::max(required, capacity * 2));
}

}
