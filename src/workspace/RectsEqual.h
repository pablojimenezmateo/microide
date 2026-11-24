#pragma once

#include <SDL3/SDL_rect.h>

namespace microide::workspace {

// Exact four-float equality for SDL_FRect: the one spelling of "the same rect"
// for layout memos, damage diffing and hover tracking alike.
inline bool RectsEqual(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

}  // namespace microide::workspace
