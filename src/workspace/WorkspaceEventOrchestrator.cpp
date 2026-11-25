#include "workspace/WorkspaceEventOrchestrator.h"

#include <utility>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

class ScopeExit {
 public:
  explicit ScopeExit(std::function<void()> on_exit) : on_exit_(std::move(on_exit)) {}

  ~ScopeExit() {
    if (on_exit_) {
      on_exit_();
    }
  }

 private:
  std::function<void()> on_exit_;
};

}

WorkspaceEventDispatcher::WorkspaceEventDispatcher(Runtime runtime,
                                                   State state,
                                                   Operations operations)
    : runtime_(runtime), state_(state), operations_(std::move(operations)) {}

EventResult WorkspaceEventDispatcher::Handle(const SDL_Event& event) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceEventDispatcher::Handle");
  const ScopeExit sync_terminal_focus{operations_.sync_terminal_focus};
  if (std::optional<EventResult> woke = HandleWakeEvent(event); woke.has_value()) {
    return std::move(*woke);
  }
  operations_.sync_text_input_surface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseButtonDown");
        return Finish(operations_.handle_mouse_button_down(event));
      }
    case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseButtonUp");
        return Finish(operations_.handle_mouse_button_up(event));
      }
    case SDL_EVENT_MOUSE_MOTION:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseMotion");
        return Finish(operations_.handle_mouse_motion(event));
      }
    case SDL_EVENT_MOUSE_WHEEL:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseWheel");
        return Finish(operations_.handle_mouse_wheel(event));
      }
    case SDL_EVENT_TEXT_EDITING:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TextEditing");
        return Finish(operations_.handle_text_editing(event.edit));
      }
    case SDL_EVENT_TEXT_INPUT:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TextInput");
        return Finish(operations_.handle_text_input(event.text));
      }
    case SDL_EVENT_DROP_FILE:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::DropFile");
        if (!operations_.handle_file_drop) {
          return Finish(false);
        }
        return Finish(operations_.handle_file_drop(event.drop.data, event.drop.x, event.drop.y));
      }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowMouseLeave");
      operations_.handle_window_mouse_leave();
      operations_.request_window_redraw();
      return Finish(true);
      }
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowMouseEnter");
      util::InvokeIfSet(operations_.force_cursor_reassert);
      operations_.request_window_redraw();
      return Finish(true);
      }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowFocusGained");
      state_.window_has_input_focus = true;
      state_.key_modifiers = SDL_GetModState();
      util::InvokeIfSet(operations_.window_focus_gained);
      util::InvokeIfSet(operations_.force_cursor_reassert);
      operations_.request_window_redraw();
      return Finish(true);
      }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowFocusLost");
      state_.window_has_input_focus = false;
      util::InvokeIfSet(operations_.end_selection_gesture);
      util::InvokeIfSet(operations_.autosave_on_focus_lost);
      util::InvokeIfSet(operations_.force_cursor_reassert);
      operations_.request_window_redraw();
      return Finish(true);
      }
    case SDL_EVENT_KEY_DOWN:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::KeyDown");
        NoteKeyModifiers(event.key.mod);
        return Finish(operations_.handle_key_down(event.key));
      }
    case SDL_EVENT_KEY_UP:
      NoteKeyModifiers(event.key.mod);
      return Finish(operations_.handle_key_up && operations_.handle_key_up(event.key));
    default:
      return Finish(false);
  }
}

EventResult WorkspaceEventDispatcher::Finish(bool handled) const {
  RenderInvalidation redraw = operations_.consume_pending_render_invalidation();
  return EventResult{
      .handled = handled || redraw.HasAnyRedraw(),
      .redraw = std::move(redraw),
  };
}

void WorkspaceEventDispatcher::NoteKeyModifiers(SDL_Keymod modifiers) const {
  const bool shift_changed = ((state_.key_modifiers ^ modifiers) & SDL_KMOD_SHIFT) != 0;
  state_.key_modifiers = modifiers;
  if (shift_changed) {
    util::InvokeIfSet(operations_.pointer_modifiers_changed);
  }
}

std::optional<EventResult> WorkspaceEventDispatcher::HandleWakeEvent(const SDL_Event& event) const {
  if (runtime_.project_open_dialog_event_type != 0 &&
      event.type == runtime_.project_open_dialog_event_type) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceEventDispatcher::Handle::ProjectOpenDialogEvent");
    operations_.consume_pending_dialog_results();
    return Finish(true);
  }
  if (operations_.plugin_runtime_consume_wake_event(event.type)) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::PluginWakeEvent");
    if (operations_.reload_plugins_if_assets_changed(true)) {
      return EventResult{.handled = true, .redraw = RenderInvalidation{.full = true, .rects = {}}};
    }
    return Finish(true);
  }
  if (runtime_.project_file_event_type != 0 && event.type == runtime_.project_file_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::ProjectFileEvent");
    util::AddPerformanceCounter(util::PerfCounterId::FileWatcherWakes);
    (void)operations_.reload_project_if_files_changed(false);
    return Finish(true);
  }
  if (operations_.project_search_handles_event(event.type)) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::ProjectSearchEvent");
    operations_.consume_project_search_updates();
    return Finish(true);
  }
  if (runtime_.lsp_event_type != 0 && event.type == runtime_.lsp_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::LspEvent");
    operations_.consume_lsp_callbacks();
    return Finish(true);
  }
  if (runtime_.dap_event_type != 0 && event.type == runtime_.dap_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::DapEvent");
    operations_.consume_dap_callbacks();
    return Finish(true);
  }
  if (runtime_.control_event_type != 0 && event.type == runtime_.control_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::ControlEvent");
    operations_.consume_control_callbacks();
    return Finish(true);
  }
  if (runtime_.plugin_thread_event_type != 0 &&
      event.type == runtime_.plugin_thread_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::PluginThreadEvent");
    operations_.consume_plugin_thread_actions();
    return Finish(true);
  }
  if (runtime_.git_blame_event_type != 0 && event.type == runtime_.git_blame_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::GitBlameEvent");
    if (operations_.apply_git_blame_results()) {
      operations_.request_frame();
    }
    return Finish(true);
  }
  if (runtime_.git_sidebar_event_type != 0 && event.type == runtime_.git_sidebar_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::GitSidebarEvent");
    operations_.consume_git_sidebar_refresh();
    return Finish(true);
  }
  if (runtime_.terminal_event_type != 0 && event.type == runtime_.terminal_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TerminalEvent");
    operations_.consume_terminal_session_updates();
    return Finish(true);
  }
  if (runtime_.highlight_prefetch_event_type != 0 &&
      event.type == runtime_.highlight_prefetch_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::HighlightPrefetchEvent");
    operations_.consume_highlight_prefetch_results();
    operations_.request_frame();
    return Finish(true);
  }

  if (runtime_.raster_decode_event_type != 0 &&
      event.type == runtime_.raster_decode_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::RasterDecodeEvent");
    operations_.apply_decoded_raster_sizes();
    return Finish(true);
  }
  return std::nullopt;
}

WorkspaceWakeController::WorkspaceWakeController(Operations operations)
    : operations_(std::move(operations)) {}

EventResult WorkspaceWakeController::HandleScheduledWake() const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceWakeController::HandleScheduledWake");
  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceScheduledWakes);
  if (operations_.reload_plugins_if_assets_changed(false)) {
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceWakeReasonPluginReload);
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }

  if (!operations_.caret_blink_animating()) {
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceWakeReasonNone);
    return {};
  }

  if (const auto caret_rect = operations_.current_caret_dirty_rect(); caret_rect.has_value()) {
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceWakeReasonCaretBlink);
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = false,
            .rects = {*caret_rect},
        },
    };
  }

  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceWakeReasonCaretBlink);
  EventResult blink{.handled = true};
  blink.redraw.frame = true;
  return blink;
}

}
