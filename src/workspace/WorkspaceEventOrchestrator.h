#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>

#include "workspace/WorkspaceEventResult.h"

namespace microide::workspace {

class WorkspaceEventDispatcher {
 public:
  struct Runtime {
    Uint32 project_open_dialog_event_type = 0;
    Uint32 git_blame_event_type = 0;
    Uint32 terminal_event_type = 0;
  };

  struct State {
    bool& window_has_input_focus;
  };

  struct Operations {
    std::function<void()> sync_terminal_focus;
    std::function<RenderInvalidation()> consume_pending_render_invalidation;
    std::function<void()> consume_pending_project_open_dialog_result;
    std::function<bool(bool)> reload_plugins_if_assets_changed;
    std::function<bool(Uint32)> plugin_runtime_consume_wake_event;
    std::function<bool(Uint32)> project_search_handles_event;
    std::function<void()> consume_project_search_updates;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> consume_terminal_session_updates;
    std::function<void(SDL_Window*)> sync_text_input_surface;
    std::function<bool(const SDL_Event&)> handle_mouse_button_down;
    std::function<bool(const SDL_Event&)> handle_mouse_button_up;
    std::function<bool(const SDL_Event&)> handle_mouse_motion;
    std::function<bool(const SDL_Event&)> handle_mouse_wheel;
    std::function<bool(const SDL_TextEditingEvent&)> handle_text_editing;
    std::function<bool(const SDL_TextInputEvent&)> handle_text_input;
    std::function<void()> request_window_redraw;
    std::function<bool(const SDL_KeyboardEvent&)> handle_key_down;
  };

  WorkspaceEventDispatcher(Runtime runtime, State state, Operations operations);

  EventResult Handle(const SDL_Event& event) const;

 private:
  Runtime runtime_;
  State state_;
  Operations operations_;
};

class WorkspaceWakeController {
 public:
  struct Operations {
    std::function<bool(bool)> reload_plugins_if_assets_changed;
    std::function<bool()> should_blink_caret;
    std::function<std::optional<SDL_FRect>()> current_caret_dirty_rect;
  };

  explicit WorkspaceWakeController(Operations operations);

  EventResult HandleScheduledWake() const;

 private:
  Operations operations_;
};

}  // namespace microide::workspace
