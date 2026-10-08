#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>

#include "workspace/services/NotificationService.h"

namespace microide::workspace {

// Toast chrome. Shared so the painter and the hit-test agree on the card by
// construction rather than by two copies of the same arithmetic — the class of
// drift that leaves chrome painted but unclickable.
inline constexpr float kNotificationToastMargin = 12.0f;
inline constexpr float kNotificationToastPadding = 10.0f;
inline constexpr float kNotificationToastGap = 8.0f;
inline constexpr float kNotificationToastAccentWidth = 3.0f;
inline constexpr float kNotificationToastMinTextWidth = 240.0f;
inline constexpr float kNotificationToastMaxTextWidth = 640.0f;
// A progress row's bar, along the bottom edge INSIDE the card. Inside rather than
// below so the card a click has to hit is the same rect whether or not the row
// reports progress — the hit-test and the painter share one geometry here
// precisely so chrome cannot end up painted and unclickable.
inline constexpr float kNotificationToastProgressHeight = 3.0f;
// A long message wraps at word boundaries onto at most this many lines (VS Code's
// toasts wrap too); only what is past the last one is truncated with "…".
inline constexpr std::size_t kNotificationMaxMessageLines = 3;

// Inline action buttons (VS Code-style): a row of them under the message, right
// aligned, each a label in a padded pill.
inline constexpr float kNotificationButtonPadX = 10.0f;
inline constexpr float kNotificationButtonPadY = 3.0f;
inline constexpr float kNotificationButtonGap = 6.0f;
inline constexpr float kNotificationButtonRowGap = 6.0f;

struct NotificationToastLayout {
  SDL_FRect rect{};
  SDL_FRect accent{};
  SDL_FRect text{};            // the message line
  SDL_FRect buttons{};         // the button row; zero-height without actions
  SDL_FRect progress_track{};  // only drawn for a row that reports progress
};

// How much message a toast may show on a window this wide. A flat cap made every
// message longer than ~40 characters land mid-word against the card edge on a
// window with room to spare, so scale with the window and keep a floor (narrow
// windows still get a readable card) and a ceiling (one long plugin/provider
// message cannot stretch the stack across the whole screen).
inline float NotificationToastTextBudget(float window_width) {
  return std::clamp(window_width * 0.45f, kNotificationToastMinTextWidth,
                    kNotificationToastMaxTextWidth);
}

// Width a toast takes for already-measured content (the wider of the message and
// the button row) on a window this wide.
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

// Where the newest toast's bottom edge sits; each older toast stacks upward from
// there, `kNotificationToastGap` above the previous one's top.
inline float NotificationStackBottom(const SDL_FRect& status_bar) {
  return status_bar.y - kNotificationToastMargin;
}

// Geometry for a toast whose bottom edge is at `bottom`. Toasts differ in height
// (a row with buttons is taller), so the stack is walked by edges, not by index.
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

}  // namespace microide::workspace
