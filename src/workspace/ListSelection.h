#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>

namespace microide::workspace {

// Standard page jump for keyboard list navigation (PageUp / PageDown) across pickers.
inline constexpr int kListPageStep = 8;

// Rows advanced per mouse-wheel tick in every scrollable *list* surface (sidebar tree/
// git/search, debug pane, bottom panel, overlay pickers, Settings rows). The editor
// text surface has always scrolled three lines per tick; the lists used to move one,
// which made the tree and the panel feel an order of magnitude heavier than the code
// next to them. One constant so they cannot drift apart again.
//
// Deliberately NOT used by: the tab strips (one tab per tick is the intent there) and
// the terminal's mouse-capture path (a captured app expects one wheel report per tick).
inline constexpr int kWheelScrollRows = 3;

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

// The vertical-navigation keys every scrollable list answers, resolved to a
// clamped move delta: Up/Down by one, Page Up/Down by kListPageStep, Home/End by
// the whole list. `count` only sizes the Home/End jump — every mover in the shell
// clamps, so a delta of the row count reliably lands on either end.
//
// Home/End as a *delta* rather than an absolute index matters for lists whose
// movers skip hidden rows (the git sidebar's collapsed directories): assigning
// index 0 there could select a row that is not on screen.
//
// Returns nullopt for anything else so callers keep their own key handling.
inline std::optional<int> ListNavigationKeyDelta(SDL_Keycode key, std::size_t count) {
  const int span = static_cast<int>(std::min<std::size_t>(
      count, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  switch (key) {
    case SDLK_UP:
      return -1;
    case SDLK_DOWN:
      return 1;
    case SDLK_PAGEUP:
      return -kListPageStep;
    case SDLK_PAGEDOWN:
      return kListPageStep;
    case SDLK_HOME:
      return -span;
    case SDLK_END:
      return span;
    default:
      return std::nullopt;
  }
}

}  // namespace microide::workspace
