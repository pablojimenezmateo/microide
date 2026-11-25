#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>

#include "workspace/services/NotificationService.h"

namespace microide::workspace {

inline constexpr float kNotificationToastMargin = 12.0f;
inline constexpr float kNotificationToastPadding = 10.0f;
inline constexpr float kNotificationToastGap = 8.0f;
inline constexpr float kNotificationToastAccentWidth = 3.0f;
inline constexpr float kNotificationToastMinTextWidth = 240.0f;
inline constexpr float kNotificationToastMaxTextWidth = 640.0f;
inline constexpr float kNotificationToastProgressHeight = 3.0f;
inline constexpr std::size_t kNotificationMaxMessageLines = 3;

inline constexpr float kNotificationButtonPadX = 10.0f;
inline constexpr float kNotificationButtonPadY = 3.0f;
inline constexpr float kNotificationButtonGap = 6.0f;
inline constexpr float kNotificationButtonRowGap = 6.0f;

struct NotificationToastLayout {
  SDL_FRect rect{};
  SDL_FRect accent{};
  SDL_FRect text{};
  SDL_FRect buttons{};
  SDL_FRect progress_track{};
};

inline float NotificationToastTextBudget(float window_width) {
  return std::clamp(window_width * 0.45f, kNotificationToastMinTextWidth,
                    kNotificationToastMaxTextWidth);
}

inline float NotificationToastWidth(float measured_content_width, float window_width) {
  return kNotificationToastAccentWidth + kNotificationToastPadding * 2.0f +
         std::min(NotificationToastTextBudget(window_width), measured_content_width);
}

inline float NotificationButtonHeight(float line_height) {
  return line_height + kNotificationButtonPadY * 2.0f;
}

inline float NotificationToastHeight(float line_height, bool has_actions, std::size_t lines = 1) {
  return line_height * static_cast<float>(std::max<std::size_t>(lines, 1)) +
         kNotificationToastPadding * 2.0f +
         (has_actions ? kNotificationButtonRowGap + NotificationButtonHeight(line_height) : 0.0f);
}

inline float NotificationStackBottom(const SDL_FRect& status_bar) {
  return status_bar.y - kNotificationToastMargin;
}

inline NotificationToastLayout NotificationToastLayoutAt(const SDL_FRect& status_bar,
                                                         float line_height,
                                                         float bottom,
                                                         float measured_content_width,
                                                         bool has_actions = false,
                                                         std::size_t message_lines = 1) {
  const std::size_t lines = std::max<std::size_t>(message_lines, 1);
  const float height = NotificationToastHeight(line_height, has_actions, lines);
  const float width = NotificationToastWidth(measured_content_width, status_bar.w);
  const SDL_FRect rect{status_bar.x + status_bar.w - kNotificationToastMargin - width,
                       bottom - height, width, height};
  const float content_x = rect.x + kNotificationToastAccentWidth + kNotificationToastPadding;
  const float content_w =
      width - kNotificationToastAccentWidth - kNotificationToastPadding * 2.0f;
  const float message_h = line_height * static_cast<float>(lines) + kNotificationToastPadding * 2.0f;
  return NotificationToastLayout{
      .rect = rect,
      .accent = SDL_FRect{rect.x, rect.y, kNotificationToastAccentWidth, rect.h},
      .text = SDL_FRect{content_x, rect.y, content_w, message_h},
      .buttons = has_actions ? SDL_FRect{content_x, rect.y + message_h - kNotificationToastPadding +
                                                        kNotificationButtonRowGap,
                                         content_w, NotificationButtonHeight(line_height)}
                             : SDL_FRect{content_x, rect.y + message_h, content_w, 0.0f},
      .progress_track =
          SDL_FRect{rect.x + kNotificationToastAccentWidth,
                    rect.y + rect.h - kNotificationToastProgressHeight,
                    rect.w - kNotificationToastAccentWidth, kNotificationToastProgressHeight},
  };
}

}
