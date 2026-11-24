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

}  // namespace

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
        // SDL owns event.drop.data and frees it after dispatch, so the handler
        // must copy anything it keeps. It does (into a std::filesystem::path).
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
      // SDL re-asserts a (possibly stale) hit-test cursor on pointer-enter; make
      // the next cursor update re-apply ours so the displayed cursor is correct.
      // Request a redraw too: the force flag is consumed inside the next prepared
      // frame, so without a queued frame (nothing else dirtied the scene) the
      // reassert would sit latent until unrelated work or motion triggered a paint.
      util::InvokeIfSet(operations_.force_cursor_reassert);
      operations_.request_window_redraw();
      return Finish(true);
      }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      {
        util::PerformanceTrace::Scope scope("WorkspaceEventDispatcher::Handle::WindowFocusGained");
      state_.window_has_input_focus = true;
      // Keys pressed or released while another window had the focus sent us nothing.
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
      // A drag does not survive the window losing focus: the button-up will be
      // delivered to whoever took the focus, not to us, so a selection left
      // "in progress" here would still be in progress on the next click. This
      // also disarms selection autoscroll, which is the one loop that keeps
      // running with no input behind it.
      util::InvokeIfSet(operations_.end_selection_gesture);
      // Autosave dirty buffers when configured for "on focus change".
      util::InvokeIfSet(operations_.autosave_on_focus_lost);
      // While unfocused another app or the compositor can repaint the global
      // cursor; force a reassert so regaining focus restores ours even if no
      // pointer-enter (which separately forces) crosses the window edge.
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
  // A handler may request a redraw (e.g. UpdateMouseCursor queuing a present so
  // the compositor re-latches the hardware cursor plane) yet still return false
  // because nothing "interesting" changed. The app loop ignores result.redraw
  // unless result.handled is set, so treat any pending redraw as handled —
  // otherwise the queued present is silently dropped and the cursor goes stale.
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

// The wake events background work posts to the main thread: each is consumed
// whole, and is never offered to the input handlers below.
std::optional<EventResult> WorkspaceEventDispatcher::HandleWakeEvent(const SDL_Event& event) const {
  if (runtime_.project_open_dialog_event_type != 0 &&
      event.type == runtime_.project_open_dialog_event_type) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceEventDispatcher::Handle::ProjectOpenDialogEvent");
    // Every native picker shares this wake event. Consuming here, not only in
    // frame prep, is what makes an answer land without some other reason to
    // paint: Open File… on a welcome page (no caret blinking) used to keep the
    // picked file until the next unrelated frame.
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
    // A watcher wake that does not lead to a reload is wasted main-thread work;
    // the wakes-vs-file_index_apply_batch_calls ratio makes an over-eager watcher
    // visible without a debugger.
    util::AddPerformanceCounter(util::PerfCounterId::FileWatcherWakes);
    // The apply requests the surfaces it changed (the sidebar, an open file's
    // pane), and `finish` collects them; it used to answer every applied batch
    // with a full frame (TD-2026-10-15-341). The event type matched, so it is ours
    // and is consumed either way — a wake whose batch turned out to change nothing
    // is still handled, not unhandled.
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
    // The blame becomes state here, with the repaint that shows it, rather than
    // whenever the worker wrote it -- which could be between two clips of one
    // partial frame (TD-2026-10-16-345).
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

  // A blinking caret that names no rect is drawn by a surface that derives its
  // own damage (an editor pane's, a compare's or a merge's caret rows): the blink
  // owes a frame, and prep damages what the phase changed -- or nothing.
  util::AddPerformanceCounter(util::PerfCounterId::WorkspaceWakeReasonCaretBlink);
  EventResult blink{.handled = true};
  blink.redraw.frame = true;
  return blink;
}

}  // namespace microide::workspace
