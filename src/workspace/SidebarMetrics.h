#pragma once

#include <SDL3/SDL.h>

namespace microide::workspace {

inline constexpr float kSidebarHeaderHeight = 26.0f;
inline constexpr float kSidebarListTopGap = 6.0f;

inline constexpr float kSidebarInset = 10.0f;

inline float SidebarListTop(const SDL_FRect& sidebar_rect) {
  return sidebar_rect.y + kSidebarHeaderHeight + kSidebarListTopGap;
}

}
