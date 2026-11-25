#pragma once

#include <limits>
#include <type_traits>

namespace microide::util {

template <typename T>
constexpr T SaturatingSub(T a, T b) {
  static_assert(std::is_unsigned_v<T>, "SaturatingSub is for unsigned types");
  return a > b ? static_cast<T>(a - b) : T{0};
}

template <typename T>
constexpr T SaturatingAdd(T a, T b) {
  static_assert(std::is_unsigned_v<T>, "SaturatingAdd is for unsigned types");
  const T limit = std::numeric_limits<T>::max();
  return b > static_cast<T>(limit - a) ? limit : static_cast<T>(a + b);
}

}
