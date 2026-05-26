#pragma once

#include <SDL3/SDL.h>

#include "workspace/WorkspaceShell.h"

namespace microide::tests {

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
