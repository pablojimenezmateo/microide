#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>

namespace microide::workspace {

inline constexpr int kListPageStep = 8;

inline constexpr int kWheelScrollRows = 3;

inline std::size_t ClampListIndexMove(std::size_t current, std::size_t count, int delta) {
  if (count == 0) {
    return 0;
  }
  const long long max_index = static_cast<long long>(count) - 1;
  const long long next = std::clamp(static_cast<long long>(current) + delta, 0LL, max_index);
  return static_cast<std::size_t>(next);
}

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

}
