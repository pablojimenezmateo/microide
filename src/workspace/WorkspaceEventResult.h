#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>

#include "util/SmallVector.h"

namespace microide::workspace {

inline constexpr std::size_t kInlineRedrawRects = 16;

struct RenderInvalidation {
  bool full = false;
  util::SmallVector<SDL_FRect, kInlineRedrawRects> rects;
  bool present = false;
  bool frame = false;

  [[nodiscard]] bool HasDamage() const { return full || !rects.empty(); }
  [[nodiscard]] bool HasAnyRedraw() const { return HasDamage() || frame || present; }

  void Add(const SDL_FRect& rect) {
    const auto covers = [](const SDL_FRect& outer, const SDL_FRect& inner) {
      return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
             inner.y + inner.h <= outer.y + outer.h;
    };
    for (const SDL_FRect& queued : rects) {
      if (covers(queued, rect)) {
        return;
      }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
      if (!covers(rect, rects[i])) {
        rects[kept++] = rects[i];
      }
    }
    rects.resize_uninitialized(kept);
    rects.push_back(rect);
  }

  void Merge(const RenderInvalidation& other) {
    present = present || other.present;
    frame = frame || other.frame;
    if (full) {
      return;
    }
    if (other.full) {
      full = true;
      rects.clear();
      return;
    }
    for (const SDL_FRect& rect : other.rects) {
      Add(rect);
    }
  }

  [[nodiscard]] std::optional<SDL_FRect> SingleRectIfOnlyOne() const {
    if (rects.size() != 1) {
      return std::nullopt;
    }
    return rects.front();
  }
};

using DamageMark = std::uint64_t;

struct EventResult {
  bool handled = false;
  RenderInvalidation redraw{};
};

}
