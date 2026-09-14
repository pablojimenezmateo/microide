#pragma once

#include <SDL3/SDL.h>

namespace microide::workspace {

// Where the sidebar's scrollable list starts, in ONE place.
//
// The layout computed `sidebar.y + 26 + 6` for each of the three list layouts,
// and the mouse coordinator subtracted its OWN `sidebar.y + 26 + 6` to turn a
// click into a row -- two copies of the same origin, magic `+ 6` included. They
// agreed, so nothing showed; a change to either would have made every sidebar
// click land on the wrong row, in silence, because both sides look correct read
// on their own.
inline constexpr float kSidebarHeaderHeight = 26.0f;
// Gap between the header and the first row.
inline constexpr float kSidebarListTopGap = 6.0f;

inline float SidebarListTop(const SDL_FRect& sidebar_rect) {
  return sidebar_rect.y + kSidebarHeaderHeight + kSidebarListTopGap;
}

}  // namespace microide::workspace
