#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <iterator>
#include <span>
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
inline bool AnyRectIntersects(std::span<const SDL_FRect> rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

// "Some dirty rect fully contains this control", which is what "the control
// repainted" actually means. Use this for every hover / button / row / tooltip
// invalidation assertion.
//
// AnyRectIntersects is close to free on those: a mouse-motion event used to emit a
// 1x1 dirty rect at the pointer (a cursor present, now the `present` bit) -- which
// was, by construction, inside the very control the test asked about -- and
// neighbouring chrome rects overlap generously. Three assertions written that way
// were found passing with the code under test compiled out.
inline bool AnyRectCovers(std::span<const SDL_FRect> rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(), [&](const SDL_FRect& rect) {
    return rect.x <= target.x && rect.y <= target.y &&
           rect.x + rect.w >= target.x + target.w && rect.y + rect.h >= target.y + target.h;
  });
}

inline float MaxRectHeight(std::span<const SDL_FRect> rects) {
  float max_height = 0.0f;
  for (const SDL_FRect& rect : rects) {
    max_height = std::max(max_height, rect.h);
  }
  return max_height;
}

// The tallest damage INSIDE `pane`: each rect clipped to it first. Damage from
// elsewhere in the same event -- a background index or git refresh landing on the
// sidebar, as tall as the window -- says nothing about what the editor repaints.
inline float InPaneDamageHeight(std::span<const SDL_FRect> rects, const SDL_FRect& pane) {
  float tallest = 0.0f;
  for (const SDL_FRect& rect : rects) {
    const float x0 = std::max(rect.x, pane.x);
    const float x1 = std::min(rect.x + rect.w, pane.x + pane.w);
    const float y0 = std::max(rect.y, pane.y);
    const float y1 = std::min(rect.y + rect.h, pane.y + pane.h);
    if (x1 > x0 && y1 > y0) {
      tallest = std::max(tallest, y1 - y0);
    }
  }
  return tallest;
}

// Event constructors for tests that dispatch a run of events and fold their
// redraws (support/RetainedRedraw.h DispatchEvents).
inline SDL_Event KeyDownEvent(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = modifiers;
  return event;
}

inline SDL_Event KeyUpEvent(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_UP;
  event.key.key = key;
  event.key.mod = modifiers;
  return event;
}

// Typed text, as SDL delivers it after a key press. `text` must outlive dispatch.
inline SDL_Event TextInputEvent(const char* text) {
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.text = text;
  return event;
}

inline SDL_Event MouseMotionEvent(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.x = x;
  event.motion.y = y;
  return event;
}

// One wheel notch per unit of `dy` (positive scrolls up), at the pointer (x, y).
inline SDL_Event MouseWheelEvent(float x, float y, float dy) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.mouse_x = x;
  event.wheel.mouse_y = y;
  event.wheel.y = dy;
  event.wheel.integer_y = static_cast<Sint32>(dy);
  return event;
}

inline SDL_Event MouseButtonEvent(Uint32 type, float x, float y) {
  SDL_Event event{};
  event.type = type;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = x;
  event.button.y = y;
  event.button.clicks = 1;
  return event;
}

// Holds `modifiers` the way a user does: their key-downs go through the shell now
// and their key-ups when this ends. A pointer handler reads the held modifiers
// from what the key events said, in queue order -- never SDL's live state, which
// this leaves alone -- so faking them with SDL_SetModState no longer reaches it.
class ScopedHeldModifiers {
 public:
  ScopedHeldModifiers(workspace::WorkspaceShell& shell, SDL_Keymod modifiers)
      : shell_(shell), modifiers_(modifiers) {
    SDL_Keymod held = SDL_KMOD_NONE;
    for (const Key& key : kKeys) {
      if ((modifiers_ & key.mask) != 0) {
        held = static_cast<SDL_Keymod>(held | (modifiers_ & key.mask));
        (void)shell_.HandleEvent(KeyDownEvent(key.key, held));
      }
    }
  }
  ~ScopedHeldModifiers() {
    SDL_Keymod held = modifiers_;
    for (auto key = std::rbegin(kKeys); key != std::rend(kKeys); ++key) {
      if ((modifiers_ & key->mask) != 0) {
        held = static_cast<SDL_Keymod>(held & ~key->mask);
        (void)shell_.HandleEvent(KeyUpEvent(key->key, held));
      }
    }
  }
  ScopedHeldModifiers(const ScopedHeldModifiers&) = delete;
  ScopedHeldModifiers& operator=(const ScopedHeldModifiers&) = delete;

 private:
  struct Key {
    SDL_Keymod mask;
    SDL_Keycode key;
  };
  static constexpr Key kKeys[] = {
      {SDL_KMOD_CTRL, SDLK_LCTRL}, {SDL_KMOD_SHIFT, SDLK_LSHIFT}, {SDL_KMOD_ALT, SDLK_LALT}};
  workspace::WorkspaceShell& shell_;
  SDL_Keymod modifiers_;
};

inline workspace::WorkspaceShell::EventResult SendKeyDownResult(workspace::WorkspaceShell& shell,
                                                                SDL_Keycode key,
                                                                SDL_Keymod modifiers) {
  return shell.HandleEvent(KeyDownEvent(key, modifiers));
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
