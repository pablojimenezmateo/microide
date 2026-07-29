#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <optional>

#include "render/TextRenderer.h"
#include "workspace/NotificationService.h"

namespace microide::workspace {

// Toast chrome. Shared so the painter and the hit-test agree on the card by
// construction rather than by two copies of the same arithmetic — the class of
// drift that leaves chrome painted but unclickable.
inline constexpr float kNotificationToastMargin = 12.0f;
inline constexpr float kNotificationToastPadding = 10.0f;
inline constexpr float kNotificationToastGap = 8.0f;
inline constexpr float kNotificationToastAccentWidth = 3.0f;
inline constexpr float kNotificationToastMaxTextWidth = 320.0f;

struct NotificationToastLayout {
  SDL_FRect rect{};
  SDL_FRect accent{};
  SDL_FRect text{};
};

// Width a toast takes for an already-measured message. Capped so one long
// plugin/provider message cannot stretch the stack across the window.
inline float NotificationToastWidth(float measured_text_width) {
  return kNotificationToastAccentWidth + kNotificationToastPadding * 2.0f +
         std::min(kNotificationToastMaxTextWidth, measured_text_width);
}

// Geometry for the toast at `stack_position`, counting 0 as the newest (the one
// sitting just above the status bar); each older toast stacks upward from there.
inline NotificationToastLayout NotificationToastLayoutAt(const SDL_FRect& status_bar,
                                                         float line_height,
                                                         std::size_t stack_position,
                                                         float measured_text_width) {
  const float height = line_height + kNotificationToastPadding * 2.0f;
  const float width = NotificationToastWidth(measured_text_width);
  const float bottom = status_bar.y - kNotificationToastMargin -
                       static_cast<float>(stack_position) * (height + kNotificationToastGap);
  const SDL_FRect rect{status_bar.x + status_bar.w - kNotificationToastMargin - width,
                       bottom - height, width, height};
  return NotificationToastLayout{
      .rect = rect,
      .accent = SDL_FRect{rect.x, rect.y, kNotificationToastAccentWidth, rect.h},
      .text = SDL_FRect{rect.x + kNotificationToastAccentWidth + kNotificationToastPadding, rect.y,
                        width - kNotificationToastAccentWidth - kNotificationToastPadding * 2.0f,
                        rect.h},
  };
}

// Index into `service.Active()` of the toast under (x, y), or nullopt. Walks the
// stack in paint order (newest first) through the same layout the painter uses, so
// a click can never land on a card that is not there.
inline std::optional<std::size_t> NotificationToastIndexAt(
    const NotificationService& service,
    const render::TextRenderer& text_renderer,
    const SDL_FRect& status_bar,
    float x,
    float y) {
  const auto& active = service.Active();
  const float line_height = text_renderer.LineHeight();
  for (std::size_t stack_position = 0; stack_position < active.size(); ++stack_position) {
    const std::size_t index = active.size() - 1 - stack_position;
    const NotificationToastLayout toast = NotificationToastLayoutAt(
        status_bar, line_height, stack_position,
        text_renderer.MeasureWidth(active[index].message));
    if (x >= toast.rect.x && x < toast.rect.x + toast.rect.w && y >= toast.rect.y &&
        y < toast.rect.y + toast.rect.h) {
      return index;
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace
