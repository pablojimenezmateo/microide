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
    Uint32 git_sidebar_event_type = 0;
    Uint32 terminal_event_type = 0;
    Uint32 project_file_event_type = 0;
    Uint32 lsp_event_type = 0;
    Uint32 dap_event_type = 0;
    Uint32 control_event_type = 0;
    Uint32 plugin_thread_event_type = 0;
    Uint32 highlight_prefetch_event_type = 0;
    Uint32 raster_decode_event_type = 0;
  };

  struct State {
    bool& window_has_input_focus;
    SDL_Keymod& key_modifiers;
  };

  struct Operations {
    std::function<void()> sync_terminal_focus;
    std::function<RenderInvalidation()> consume_pending_render_invalidation;
    std::function<void()> consume_pending_dialog_results;
    std::function<bool(bool)> reload_plugins_if_assets_changed;
    std::function<bool(Uint32)> plugin_runtime_consume_wake_event;
    std::function<bool(bool)> reload_project_if_files_changed;
    std::function<bool(Uint32)> project_search_handles_event;
    std::function<void()> consume_project_search_updates;
    std::function<void()> request_frame;
    std::function<void()> consume_git_sidebar_refresh;
    std::function<bool()> apply_git_blame_results;
    std::function<void()> consume_highlight_prefetch_results;
    std::function<void()> apply_decoded_raster_sizes;
    std::function<void()> consume_lsp_callbacks;
    std::function<void()> consume_dap_callbacks;
    std::function<void()> consume_control_callbacks;
    std::function<void()> consume_plugin_thread_actions;
    std::function<void()> consume_terminal_session_updates;
    std::function<void(SDL_Window*)> sync_text_input_surface;
    std::function<bool(const SDL_Event&)> handle_mouse_button_down;
    std::function<bool(const SDL_Event&)> handle_mouse_button_up;
    std::function<bool(const SDL_Event&)> handle_mouse_motion;
    std::function<bool(const SDL_Event&)> handle_mouse_wheel;
    std::function<bool(const SDL_TextEditingEvent&)> handle_text_editing;
    std::function<bool(const SDL_TextInputEvent&)> handle_text_input;
    std::function<void()> handle_window_mouse_leave;
    std::function<void()> force_cursor_reassert;
    std::function<void()> window_focus_gained;
    std::function<void()> pointer_modifiers_changed;
    std::function<void()> autosave_on_focus_lost;
    std::function<void()> end_selection_gesture;
    std::function<void()> request_window_redraw;
    std::function<bool(const SDL_KeyboardEvent&)> handle_key_down;
    std::function<bool(const SDL_KeyboardEvent&)> handle_key_up;
    std::function<bool(const char*, float, float)> handle_file_drop;
  };

  WorkspaceEventDispatcher(Runtime runtime, State state, Operations operations);

  EventResult Handle(const SDL_Event& event) const;

 private:
  std::optional<EventResult> HandleWakeEvent(const SDL_Event& event) const;
  EventResult Finish(bool handled) const;
  void NoteKeyModifiers(SDL_Keymod modifiers) const;

  Runtime runtime_;
  State state_;
  Operations operations_;
};

class WorkspaceWakeController {
 public:
  struct Operations {
    std::function<bool(bool)> reload_plugins_if_assets_changed;
    std::function<bool()> caret_blink_animating;
    std::function<std::optional<SDL_FRect>()> current_caret_dirty_rect;
  };

  explicit WorkspaceWakeController(Operations operations);

  EventResult HandleScheduledWake() const;

 private:
  Operations operations_;
};

}
