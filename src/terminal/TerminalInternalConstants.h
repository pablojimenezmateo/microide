#pragma once

#include <cstddef>

namespace microide::terminal {

inline constexpr std::size_t kMaxScrollbackLines = 2000;


inline void ClampPendingWrapColumn(std::size_t& column, std::size_t columns) {
  if (columns > 0 && column >= columns) {
    column = columns - 1;
  }
}

}
