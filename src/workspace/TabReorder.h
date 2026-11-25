#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace microide::workspace {

template <typename T>
bool ReorderActive(std::vector<T>& vec, std::size_t& active, std::size_t target) {
  if (active >= vec.size() || target >= vec.size()) {
    return false;
  }
  if (active == target) {
    return true;
  }
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

}
