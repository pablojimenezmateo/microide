// Session-callback wiring and stop projection for DebugService, split out of
// DebugService.cpp to keep that translation unit thin. BuildSessionCallbacks is
// the large factory that binds a DebugSession's events to the shared project
// views; ProjectStop projects a resolved stop into those views and
// ClearTransientDebugViews tears them down on resume/teardown. These are still
// DebugService members; only their definitions (and the two prebuilt-view
// helpers they use) live here.
#include "workspace/DebugService.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

// Build the host-side execution view from a stop + its resolved frames. Display
// strings are prebuilt here (off the render hot path) so render TUs only draw.
DebugExecutionView BuildExecutionView(const dap_protocol::DapStoppedEvent& stop,
                                      const std::vector<dap_protocol::DapStackFrame>& frames) {
  DebugExecutionView view;
  view.stopped = true;
  view.thread_id = stop.thread_id;
  view.stop_reason = stop.reason;
  view.focused_frame_index = 0;
  view.frames.reserve(frames.size());
  for (const dap_protocol::DapStackFrame& frame : frames) {
    DebugStackFrameView row;
    row.id = frame.id;
    if (!frame.source.path.empty()) {
      row.source_path = std::filesystem::path(frame.source.path).lexically_normal();
    }
    // DAP lines are 1-based; the editor buffer is 0-based.
    row.line = frame.line > 0 ? static_cast<std::size_t>(frame.line - 1) : 0;
    row.display_primary = frame.name;
    std::string location =
        !frame.source.name.empty()
            ? frame.source.name
            : (row.source_path.empty() ? std::string() : row.source_path.filename().string());
    if (!location.empty() && frame.line > 0) {
      location += ':';
      location += std::to_string(frame.line);
    }
    row.display_secondary = std::move(location);
    view.frames.push_back(std::move(row));
  }
  return view;
}

// Build the thread-selector rows, prebuilding each display string off the render
// hot path (e.g. "Thread 1: MainThread").
std::vector<DebugThreadView> BuildThreadViews(const std::vector<dap_protocol::DapThread>& threads) {
  std::vector<DebugThreadView> views;
  views.reserve(threads.size());
  for (const dap_protocol::DapThread& thread : threads) {
    DebugThreadView row;
    row.id = thread.id;
    std::string display = "Thread " + std::to_string(thread.id);
    if (!thread.name.empty()) {
      display += ": ";
      display += thread.name;
    }
    row.display = std::move(display);
    views.push_back(std::move(row));
  }
  return views;
}

}  // namespace

void DebugService::ProjectStop(const dap_protocol::DapStoppedEvent& stop,
                               const std::vector<dap_protocol::DapStackFrame>& frames) {
  ProjectWorkspaceState& state = CurrentProjectState();
  // Preserve the thread + session selectors across the rebuild (frames are rebuilt
  // for the stopped thread, but the selector contents do not change).
  std::vector<DebugThreadView> threads = std::move(state.debug_execution.threads);
  std::vector<DebugSessionView> sessions = std::move(state.debug_execution.sessions);
  const int focused_session = state.debug_execution.focused_session_id;
  state.debug_execution = BuildExecutionView(stop, frames);
  state.debug_execution.threads = std::move(threads);
  state.debug_execution.focused_thread_id = stop.thread_id;
  state.debug_execution.sessions = std::move(sessions);
  state.debug_execution.focused_session_id = focused_session;
  // A fresh stop invalidates any prior hover value (new frame state / values).
  state.debug_hover.Clear();
  // Populate the Variables tree for the top (focused) frame so it is ready the
  // instant the user switches to the Variables tab (scopes is one cheap request).
  if (const DebugStackFrameView* focused = state.debug_execution.FocusedFrame();
      focused != nullptr) {
    FocusFrame(focused->id);
  }
  if (operations_.show_call_stack_panel) {
    operations_.show_call_stack_panel();
  }
  if (operations_.focus_source_location && state.debug_execution.HasLocation()) {
    operations_.focus_source_location(state.debug_execution.FocusedPath(),
                                      state.debug_execution.FocusedLine());
  }
  if (operations_.request_editor_redraw) {
    operations_.request_editor_redraw();
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
  // The execution view is now fully populated (file/line/frames): report the
  // resolved stop to push observers. ProjectStop runs only for the active
  // session, so this fires exactly once per visible stop.
  if (operations_.notify_stop_resolved) {
    operations_.notify_stop_resolved();
  }
}

DebugSession::Callbacks DebugService::BuildSessionCallbacks(int session_id,
                                                            std::string session_label) {
  DebugSession::Callbacks callbacks;
  callbacks.on_output = [this, session_id, session_label](
                            const dap_protocol::DapOutputEvent& output) {
    // Each session streams to its own console channel, regardless of which session
    // is active, so background output never intermixes with the foreground one.
    if (operations_.append_console_output) {
      operations_.append_console_output(session_id, session_label, output);
    }
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  callbacks.on_state_changed = [this](DebugSession::State state) {
    if (operations_.notify_session_state_changed) {
      operations_.notify_session_state_changed(state);
    }
    // A state change moves a session row's label (running/paused/terminated).
    SyncSessionsPanel();
    if (operations_.request_chrome_redraw) {
      operations_.request_chrome_redraw();
    }
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  callbacks.on_capabilities_changed = [this]() {
    // A late `capabilities` event can flip supportsStepBack on (gdb under rr/record),
    // which adds the Reverse Continue / Step Back buttons to the floating debug
    // toolbar (chrome). Repaint so they appear without waiting for the next input.
    if (operations_.request_chrome_redraw) {
      operations_.request_chrome_redraw();
    }
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  // Fired once on the terminal transition (incl. a crash/kill, which reaches Failed
  // without a DAP `terminated`). Drives the control-channel broadcast for every end.
  callbacks.on_terminated = [this, session_id](DebugSession::State terminal_state,
                                               const std::string& reason) {
    if (operations_.notify_session_terminated) {
      operations_.notify_session_terminated(session_id,
                                            terminal_state == DebugSession::State::Failed, reason);
    }
  };
  // Immediate halt notification (real reason/thread, before frames resolve).
  // Only the active session drives the shared push broadcast — a background stop
  // does not project into the shared views, so it must not report one either.
  callbacks.on_stop_began = [this, session_id](const dap_protocol::DapStoppedEvent& stop) {
    if (IsActiveSession(session_id) && operations_.notify_stop_began) {
      operations_.notify_stop_began(stop.reason, stop.thread_id);
    }
  };
  // On every stop: the active session projects into the shared views; a background
  // session badges for attention and (when the user is not parked at another stop)
  // auto-focuses so the just-paused session comes to the foreground.
  callbacks.on_stopped = [this, session_id](
                             const dap_protocol::DapStoppedEvent& stop,
                             const std::vector<dap_protocol::DapStackFrame>& frames) {
    DapManager& manager = CurrentDapManager();
    if (IsActiveSession(session_id)) {
      manager.SetSessionAttention(session_id, false);
      ProjectStop(stop, frames);
      SyncSessionsPanel();
      return;
    }
    manager.SetSessionAttention(session_id, true);
    const DebugSession* active = manager.ActiveSession();
    const bool active_inspecting =
        active != nullptr && active->CurrentState() == DebugSession::State::Stopped;
    if (!active_inspecting) {
      // Auto-focus: bring the just-paused background session to the foreground.
      manager.SetActiveSession(session_id);
      manager.SetSessionAttention(session_id, false);
      ClearTransientDebugViews();
      if (operations_.show_debug_console) {
        operations_.show_debug_console(session_id, manager.SessionLabel(session_id));
      }
      ProjectStop(stop, frames);
    }
    SyncSessionsPanel();
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  // On resume: the active session drops its execution view; a background resume
  // just clears that session's attention badge.
  callbacks.on_resumed = [this, session_id]() {
    if (IsActiveSession(session_id)) {
      ClearTransientDebugViews();
    } else {
      CurrentDapManager().SetSessionAttention(session_id, false);
      SyncSessionsPanel();
      if (operations_.request_debug_pane_redraw) {
        operations_.request_debug_pane_redraw();
      }
    }
  };
  // Thread list lands a beat after the stop; fill the selector (preserving the
  // focused thread). Only the active session drives the shared selector.
  callbacks.on_threads = [this, session_id](
                             const std::vector<dap_protocol::DapThread>& threads) {
    ProjectWorkspaceState& state = CurrentProjectState();
    if (!IsActiveSession(session_id) || !state.debug_execution.stopped) {
      return;
    }
    state.debug_execution.threads = BuildThreadViews(threads);
    if (state.debug_execution.focused_thread_id == 0) {
      state.debug_execution.focused_thread_id = state.debug_execution.thread_id;
    }
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  // The session pulls the breakpoint snapshot at `initialized` and on each live
  // re-send; verification reflects back into the project's BreakpointStore.
  callbacks.breakpoint_provider = [this]() {
    return CurrentProjectState().breakpoint_store.SnapshotAll();
  };
  callbacks.on_breakpoints_verified =
      [this](const std::filesystem::path& path, const std::vector<int>& requested_lines,
             const std::vector<dap_protocol::DapBreakpoint>& breakpoints) {
        std::vector<editor::VerifiedBreakpoint> results;
        results.reserve(breakpoints.size());
        for (std::size_t i = 0; i < breakpoints.size(); ++i) {
          const dap_protocol::DapBreakpoint& breakpoint = breakpoints[i];
          // The response is positional to the request, so match by the line we
          // requested at this index (the store may have changed while in flight).
          // Fall back to the adapter-reported line for a non-conformant adapter
          // that returns a shorter array.
          const int match_line =
              i < requested_lines.size() ? requested_lines[i] : breakpoint.line;
          results.push_back(editor::VerifiedBreakpoint{
              .id = breakpoint.id,
              .verified = breakpoint.verified,
              .line = match_line,
              .message = breakpoint.message,
          });
        }
        CurrentProjectState().breakpoint_store.ApplyVerification(path, results);
        SyncBreakpointsPanel();
        if (operations_.request_editor_redraw) {
          operations_.request_editor_redraw();
        }
      };
  callbacks.on_breakpoint_changed = [this](const std::filesystem::path& path,
                                           const dap_protocol::DapBreakpoint& breakpoint) {
    CurrentProjectState().breakpoint_store.ApplyBreakpointEvent(
        path, editor::VerifiedBreakpoint{
                  .id = breakpoint.id,
                  .verified = breakpoint.verified,
                  .line = breakpoint.line,
                  .message = breakpoint.message,
              });
    // A function breakpoint binds asynchronously too (gdb reports it `pending` in the
    // setFunctionBreakpoints response, then verifies it via a `breakpoint` event).
    // Match by adapter id; a line event simply finds no function breakpoint.
    CurrentProjectState().function_breakpoint_store.ApplyBreakpointEvent(
        editor::VerifiedFunctionBreakpoint{
            .id = breakpoint.id,
            .verified = breakpoint.verified,
            .message = breakpoint.message,
        });
    SyncBreakpointsPanel();
    if (operations_.request_editor_redraw) {
      operations_.request_editor_redraw();
    }
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  };
  // The session pulls the function-breakpoint snapshot at `initialized` and on each
  // live re-send; verification reflects back into the FunctionBreakpointStore.
  callbacks.function_breakpoint_provider = [this]() {
    return CurrentProjectState().function_breakpoint_store.All();
  };
  callbacks.on_function_breakpoints_verified =
      [this](const std::vector<std::string>& requested_names,
             const std::vector<dap_protocol::DapBreakpoint>& breakpoints) {
        std::vector<editor::VerifiedFunctionBreakpoint> results;
        results.reserve(breakpoints.size());
        for (const dap_protocol::DapBreakpoint& breakpoint : breakpoints) {
          results.push_back(editor::VerifiedFunctionBreakpoint{
              .id = breakpoint.id,
              .verified = breakpoint.verified,
              .message = breakpoint.message,
          });
        }
        CurrentProjectState().function_breakpoint_store.ApplyVerification(requested_names, results);
        SyncBreakpointsPanel();
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      };
  // Advertised exception filters arrive at `initialized`: populate the Breakpoints
  // panel (seeding the enabled set from adapter defaults the first time).
  callbacks.on_exception_filters_available =
      [this](const std::vector<dap_protocol::DapExceptionFilter>& filters) {
        CurrentProjectState().debug_breakpoints_panel.SetAdvertisedFilters(filters);
        SyncBreakpointsPanel();
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      };
  // The session pulls the enabled filters (id + optional condition) at `initialized`
  // and on live re-send.
  callbacks.exception_filter_provider = [this]() {
    std::vector<ExceptionFilterRequest> requests;
    for (const auto& [id, condition] :
         CurrentProjectState().debug_breakpoints_panel.EnabledFilterOptions()) {
      requests.push_back(ExceptionFilterRequest{.id = id, .condition = condition});
    }
    return requests;
  };
  return callbacks;
}

void DebugService::ClearTransientDebugViews() {
  // Invalidate any in-flight scopes/variables/setVariable/watch responses: they
  // belong to the stop being torn down and must not apply to the cleared models.
  frame_generation_.bump();
  watch_generation_.bump();
  ProjectWorkspaceState& state = CurrentProjectState();
  state.debug_execution.Clear();
  state.debug_variables.Clear();
  state.debug_hover.Clear();
  // Keep the watch expressions (persistent) but drop their stale values.
  state.debug_watch.ClearResults();
  if (operations_.request_editor_redraw) {
    operations_.request_editor_redraw();
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

}  // namespace microide::workspace
