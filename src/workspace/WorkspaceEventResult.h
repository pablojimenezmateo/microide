#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <vector>

namespace microide::workspace {

struct RenderInvalidation {
  bool full = false;
  std::vector<SDL_FRect> rects;

  [[nodiscard]] bool HasAnyRedraw() const { return full || !rects.empty(); }

  [[nodiscard]] std::optional<SDL_FRect> SingleRectIfOnlyOne() const {
    if (rects.size() != 1) {
      return std::nullopt;
    }
    return rects.front();
  }
};

struct EventResult {
  bool handled = false;
  RenderInvalidation redraw{};
};

}  // namespace microide::workspace
