#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "editor/EditTypes.h"

namespace microide::editor {

// The application order for a batch of ranged edits whose coordinates all refer
// to the SAME pre-edit document (an LSP `TextEdit[]`, a plugin `apply_edits`
// request, a formatter result).
//
// Highest position first, so applying an edit never shifts the coordinates of an
// edit still to come. Ties follow VS Code's `Range.compareRangesUsingStarts` plus
// its stable sort index, read in reverse: same start → the LONGER range applies
// first (a replace at P must land before any insert at P, or the insert's text
// sits inside the replaced span and is deleted with it), and same range → the
// LATER array entry applies first, so N inserts at one position come out in
// array order left to right (each applied insert pushes the earlier one right).
//
// Three appliers used to carry their own copy of the sort; one of them was a
// plain `std::sort` keyed on the start alone, which reverses two same-position
// inserts on every libstdc++ (its small-range insertion sort is stable) and is
// unspecified elsewhere.
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

// True when two ranges of `ranges` intersect (share at least one pre-edit byte).
// Touching endpoints are not an overlap: an insert at P beside a replace of
// [P, Q) is the shape LSP explicitly permits ("any number of inserts followed by
// a single remove or replace edit"). `order` must come from
// `OrderEditsForApplication` over the same `ranges`.
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
    // Descending order: `hi` starts at or after `lo`. They intersect when `lo`
    // ends past `hi`'s start.
    if (lo.end.line > hi.start.line ||
        (lo.end.line == hi.start.line && lo.end.column > hi.start.column)) {
      return true;
    }
  }
  return false;
}

}  // namespace microide::editor
