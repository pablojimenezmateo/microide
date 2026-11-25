#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "editor/EditTypes.h"

namespace microide::editor {

inline void OrderEditsForApplication(std::span<const SelectionRange> ranges,
                                     std::vector<std::size_t>& order) {
  order.resize(ranges.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  const auto normalized = [&](std::size_t index) -> SelectionRange {
    SelectionRange range = ranges[index];
    const bool reversed = range.end.line < range.start.line ||
                          (range.end.line == range.start.line && range.end.column < range.start.column);
    if (reversed) {
      std::swap(range.start, range.end);
    }
    return range;
  };
  const auto before = [](const TextPosition& a, const TextPosition& b) {
    return a.line < b.line || (a.line == b.line && a.column < b.column);
  };
  std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    const SelectionRange a = normalized(lhs);
    const SelectionRange b = normalized(rhs);
    if (before(b.start, a.start)) return true;
    if (before(a.start, b.start)) return false;
    if (before(b.end, a.end)) return true;
    if (before(a.end, b.end)) return false;
    return lhs > rhs;
  });
}

inline bool EditsOverlap(std::span<const SelectionRange> ranges,
                         std::span<const std::size_t> order) {
  const auto normalized = [&](std::size_t index) -> SelectionRange {
    SelectionRange range = ranges[index];
    const bool reversed = range.end.line < range.start.line ||
                          (range.end.line == range.start.line && range.end.column < range.start.column);
    if (reversed) {
      std::swap(range.start, range.end);
    }
    return range;
  };
  for (std::size_t i = 1; i < order.size(); ++i) {
    const SelectionRange hi = normalized(order[i - 1]);
    const SelectionRange lo = normalized(order[i]);
    if (lo.end.line > hi.start.line ||
        (lo.end.line == hi.start.line && lo.end.column > hi.start.column)) {
      return true;
    }
  }
  return false;
}

}
