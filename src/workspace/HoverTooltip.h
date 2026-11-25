#pragma once

#include <SDL3/SDL_rect.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "workspace/RectsEqual.h"

namespace microide::workspace {

struct HoverTooltip {
  std::string text;
  SDL_FRect rect{};
};

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

}
