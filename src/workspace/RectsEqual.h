#pragma once

#include <SDL3/SDL_rect.h>

namespace microide::workspace {

inline bool RectsEqual(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

}
