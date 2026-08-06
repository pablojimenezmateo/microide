#pragma once

#include <SDL3/SDL.h>

#include <optional>

#include "util/SmallVector.h"

namespace microide::workspace {

// Damage rects queued for one event, inline up to `kInlineRedrawRects`.
//
// This is on every input event: a mouse move queues one rect, the shell copies
// the struct out and the app loop appends it to the frame's damage list, and
// with a `std::vector` that was two heap round-trips per event to carry 16
// bytes (TD-2026-08-06-145). Eight is measured, not guessed: with the counters
// below armed, `menu_hover_switch` queues 7 rects per event (1,120 over 160
// events) and every other interactive scenario stays at or under 4. Anything
// past it spills to the heap rather than being dropped, and each spill bumps
// `workspace.redraw_rect_spills` — so if a new surface starts queuing more, the
// wrong guess is visible in the gate rather than silently costing a malloc per
// event again.
inline constexpr std::size_t kInlineRedrawRects = 8;

struct RenderInvalidation {
  bool full = false;
  util::SmallVector<SDL_FRect, kInlineRedrawRects> rects;

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
