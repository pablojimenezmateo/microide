#pragma once

#include <SDL3/SDL_rect.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace microide::workspace {

// One resolved hover tooltip: the (already truncated) text to draw and the card
// rect it goes in. The shell resolves at most one of these per frame through
// WorkspaceShell::HoveredTooltip, which owns the provider order and the
// placement rule, so render, cursor feedback and redraw invalidation all agree
// on what is showing and where.
struct HoverTooltip {
  std::string text;
  SDL_FRect rect{};
};

inline bool SameTooltipRect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

// The shell's one tooltip placement rule: horizontally centered on the control
// the tooltip describes, below it when the card fits, flipped above it when it
// does not (the status bar sits on the window's bottom edge), and clamped inside
// `bounds` either way. Anchoring to the control rather than to the pointer is
// what keeps a tooltip still while the pointer moves within one button.
inline SDL_FRect PlaceTooltipCard(const SDL_FRect& anchor,
                                  float width,
                                  float height,
                                  const SDL_FRect& bounds) {
  constexpr float kMargin = 8.0f;
  constexpr float kGap = 6.0f;
  const float min_x = bounds.x + kMargin;
  const float max_x = std::max(min_x, bounds.x + bounds.w - width - kMargin);
  const float x = std::clamp(anchor.x + anchor.w * 0.5f - width * 0.5f, min_x, max_x);

  const float below_y = anchor.y + anchor.h + kGap;
  const float above_y = anchor.y - kGap - height;
  const bool fits_below = below_y + height <= bounds.y + bounds.h - kMargin;
  const bool fits_above = above_y >= bounds.y + kMargin;
  const float min_y = bounds.y + kMargin;
  const float max_y = std::max(min_y, bounds.y + bounds.h - height - kMargin);
  const float y = std::clamp(fits_below || !fits_above ? below_y : above_y, min_y, max_y);

  return SDL_FRect{std::floor(x), std::floor(y), width, height};
}

}  // namespace microide::workspace
