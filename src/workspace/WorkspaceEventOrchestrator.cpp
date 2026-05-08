#include "workspace/WorkspaceEventOrchestrator.h"

#include <utility>

#include "util/PerformanceTrace.h"

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

}  // namespace

WorkspaceEventDispatcher::WorkspaceEventDispatcher(Runtime runtime,
                                                   State state,
                                                   Operations operations)
    : runtime_(runtime), state_(state), operations_(std::move(operations)) {}

EventResult WorkspaceEventDispatcher::Handle(const SDL_Event& event) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceEventDispatcher::Handle");
  const ScopeExit sync_terminal_focus{operations_.sync_terminal_focus};
  const auto finish = [this](bool handled) {
    return EventResult{
        .handled = handled,
        .redraw = operations_.consume_pending_render_invalidation(),
    };
  };

  if (runtime_.project_open_dialog_event_type != 0 &&
      event.type == runtime_.project_open_dialog_event_type) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceEventDispatcher::Handle::ProjectOpenDialogEvent");
    operations_.consume_pending_project_open_dialog_result();
    return finish(true);
  }
  if (operations_.plugin_runtime_consume_wake_event(event.type)) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::PluginWakeEvent");
    if (operations_.reload_plugins_if_assets_changed(true)) {
      return EventResult{
          .handled = true,
          .redraw = RenderInvalidation{
              .full = true,
              .rects = {},
          },
      };
    }
    return finish(true);
  }
  if (runtime_.project_file_event_type != 0 && event.type == runtime_.project_file_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::ProjectFileEvent");
    const bool consumed = operations_.project_file_monitor_consume_wake_event(event.type);
    const bool reloaded = operations_.reload_project_if_files_changed(false);
    if (reloaded) {
      return EventResult{
          .handled = true,
          .redraw = RenderInvalidation{
              .full = true,
              .rects = {},
          },
      };
    }
    return finish(consumed || reloaded);
  }
  if (operations_.project_search_handles_event(event.type)) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::ProjectSearchEvent");
    operations_.consume_project_search_updates();
    return finish(true);
  }
  if (operations_.ai_runtime_handles_event(event.type)) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::AiRuntimeEvent");
    operations_.consume_ai_runtime_updates();
    return finish(true);
  }
  if (runtime_.lsp_event_type != 0 && event.type == runtime_.lsp_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::LspEvent");
    operations_.consume_lsp_callbacks();
    return finish(true);
  }
  if (runtime_.plugin_async_process_event_type != 0 &&
      event.type == runtime_.plugin_async_process_event_type) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceEventDispatcher::Handle::PluginAsyncProcessEvent");
    operations_.consume_plugin_async_process_callbacks();
    return finish(true);
  }
  if (runtime_.git_blame_event_type != 0 && event.type == runtime_.git_blame_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::GitBlameEvent");
    operations_.request_focused_editor_redraw();
    return finish(true);
  }
  if (runtime_.git_sidebar_event_type != 0 && event.type == runtime_.git_sidebar_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::GitSidebarEvent");
    operations_.consume_git_sidebar_refresh();
    return finish(true);
  }
  if (runtime_.terminal_event_type != 0 && event.type == runtime_.terminal_event_type) {
    util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TerminalEvent");
    operations_.consume_terminal_session_updates();
    return finish(true);
  }

  operations_.sync_text_input_surface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseButtonDown");
        return finish(operations_.handle_mouse_button_down(event));
      }
    case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseButtonUp");
        return finish(operations_.handle_mouse_button_up(event));
      }
    case SDL_EVENT_MOUSE_MOTION:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseMotion");
        return finish(operations_.handle_mouse_motion(event));
      }
    case SDL_EVENT_MOUSE_WHEEL:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::MouseWheel");
        return finish(operations_.handle_mouse_wheel(event));
      }
    case SDL_EVENT_TEXT_EDITING:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TextEditing");
        return finish(operations_.handle_text_editing(event.edit));
      }
    case SDL_EVENT_TEXT_INPUT:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::TextInput");
        return finish(operations_.handle_text_input(event.text));
      }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowMouseLeave");
      operations_.handle_window_mouse_leave();
      operations_.request_window_redraw();
      return finish(true);
      }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowFocusGained");
      state_.window_has_input_focus = true;
      operations_.request_window_redraw();
      return finish(true);
      }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowFocusLost");
      state_.window_has_input_focus = false;
      operations_.request_window_redraw();
      return finish(true);
      }
    case SDL_EVENT_KEY_DOWN:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::KeyDown");
        return finish(operations_.handle_key_down(event.key));
      }
    default:
      return finish(false);
  }
}

WorkspaceWakeController::WorkspaceWakeController(Operations operations)
    : operations_(std::move(operations)) {}

EventResult WorkspaceWakeController::HandleScheduledWake() const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceWakeController::HandleScheduledWake");
  if (operations_.reload_plugins_if_assets_changed(false)) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = true,
            .rects = {},
        },
    };
  }

  if (!operations_.should_blink_caret()) {
    return {};
  }

  if (const auto caret_rect = operations_.current_caret_dirty_rect(); caret_rect.has_value()) {
    return EventResult{
        .handled = true,
        .redraw = RenderInvalidation{
            .full = false,
            .rects = {*caret_rect},
        },
    };
  }

  // No visible caret region means there is nothing to repaint for caret blink.
  return {};
}

}  // namespace microide::workspace
