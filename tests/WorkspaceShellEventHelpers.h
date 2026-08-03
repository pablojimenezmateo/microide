#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests {

// Partial-redraw assertions. These were six near-identical private copies across
// the shell test files; they live here so a redraw test in one file cannot mean
// something different from the same-named test in another.

inline bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

// "Some dirty rect touches this surface." Correct for a large surface that is
// only PARTLY repainted (an edited line inside the editor pane). Do NOT use it to
// assert that a specific small control repainted -- see AnyRectCovers.
inline bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

// "Some dirty rect fully contains this control", which is what "the control
// repainted" actually means. Use this for every hover / button / row / tooltip
// invalidation assertion.
//
// AnyRectIntersects is close to free on those: a mouse-motion event already emits
// a 1x1 dirty rect at the pointer -- which is, by construction, inside the very
// control the test is asking about -- and neighbouring chrome rects overlap
// generously. Three assertions written that way were found passing with the code
// under test compiled out.
inline bool AnyRectCovers(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(), [&](const SDL_FRect& rect) {
    return rect.x <= target.x && rect.y <= target.y &&
           rect.x + rect.w >= target.x + target.w && rect.y + rect.h >= target.y + target.h;
  });
}

inline float MaxRectHeight(const std::vector<SDL_FRect>& rects) {
  float max_height = 0.0f;
  for (const SDL_FRect& rect : rects) {
    max_height = std::max(max_height, rect.h);
  }
  return max_height;
}

inline workspace::WorkspaceShell::EventResult SendKeyDownResult(workspace::WorkspaceShell& shell,
                                                                SDL_Keycode key,
                                                                SDL_Keymod modifiers) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = modifiers;
  return shell.HandleEvent(event);
}

inline bool SendKeyDown(workspace::WorkspaceShell& shell, SDL_Keycode key, SDL_Keymod modifiers) {
  return SendKeyDownResult(shell, key, modifiers).handled;
}

inline bool SendMouseDown(workspace::WorkspaceShell& shell,
                          float x,
                          float y,
                          Uint8 button,
                          Uint8 clicks = 1) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = button;
  event.button.x = x;
  event.button.y = y;
  event.button.clicks = clicks;
  return shell.HandleEvent(event).handled;
}

inline bool SendMouseUp(workspace::WorkspaceShell& shell, float x, float y, Uint8 button) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = button;
  event.button.x = x;
  event.button.y = y;
  return shell.HandleEvent(event).handled;
}

inline bool SendMouseMotion(workspace::WorkspaceShell& shell,
                            float x,
                            float y,
                            SDL_MouseButtonFlags state) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.x = x;
  event.motion.y = y;
  event.motion.state = state;
  return shell.HandleEvent(event).handled;
}

inline bool SendMouseWheel(workspace::WorkspaceShell& shell,
                           float x,
                           float y,
                           int vertical_ticks,
                           int horizontal_ticks = 0) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.mouse_x = x;
  event.wheel.mouse_y = y;
  event.wheel.integer_x = horizontal_ticks;
  event.wheel.integer_y = vertical_ticks;
  event.wheel.x = static_cast<float>(horizontal_ticks);
  event.wheel.y = static_cast<float>(vertical_ticks);
  return shell.HandleEvent(event).handled;
}

inline bool SendWindowFocus(workspace::WorkspaceShell& shell, bool focused) {
  SDL_Event event{};
  event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
  return shell.HandleEvent(event).handled;
}

inline bool SendWindowMouseLeave(workspace::WorkspaceShell& shell) {
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
  return shell.HandleEvent(event).handled;
}

inline bool SendWindowResized(workspace::WorkspaceShell& shell, int width, int height) {
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_RESIZED;
  event.window.data1 = width;
  event.window.data2 = height;
  return shell.HandleEvent(event).handled;
}

}  // namespace microide::tests
