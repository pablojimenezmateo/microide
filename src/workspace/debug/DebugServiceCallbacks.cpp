#include "workspace/debug/DebugService.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "workspace/WorkspaceContext.h"
#include "workspace/debug/DebugFrameFocus.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

DebugExecutionView BuildExecutionView(const dap_protocol::DapStoppedEvent& stop,
                                      const std::vector<dap_protocol::DapStackFrame>& frames) {
  DebugExecutionView view;
  view.stopped = true;
  view.thread_id = stop.thread_id.value_or(0);
  view.stop_reason = stop.reason;
  view.focused_frame_index = TopFrameWithSource(frames, [](const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
  });
  view.frames.reserve(frames.size());
  for (const dap_protocol::DapStackFrame& frame : frames) {
    DebugStackFrameView row;
    row.id = frame.id;
    row.SetSource(frame.source.path);
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

}

void DebugService::ProjectStop(const dap_protocol::DapStoppedEvent& stop,
                               const std::vector<dap_protocol::DapStackFrame>& frames) {
  ProjectWorkspaceState& state = CurrentProjectState();
  std::vector<DebugThreadView> threads = std::move(state.debug_execution.threads);
  std::vector<DebugSessionView> sessions = std::move(state.debug_execution.sessions);
  const int focused_session = state.debug_execution.focused_session_id;
  state.debug_execution = BuildExecutionView(stop, frames);
  state.debug_execution.threads = std::move(threads);
  state.debug_execution.focused_thread_id = stop.thread_id.value_or(0);
  state.debug_execution.sessions = std::move(sessions);
  state.debug_execution.focused_session_id = focused_session;
  state.debug_hover.Clear();
  if (const DebugStackFrameView* focused = state.debug_execution.FocusedFrame();
      focused != nullptr) {
    FocusFrame(focused->id);
  }
  util::InvokeIfSet(operations_.show_call_stack_panel);
  if (operations_.focus_source_location && state.debug_execution.HasLocation()) {
    operations_.focus_source_location(state.debug_execution.FocusedPath(),
                                      state.debug_execution.FocusedLine());
  }
  util::InvokeIfSet(operations_.request_frame);
  util::InvokeIfSet(operations_.notify_stop_resolved);
}

DebugSession::Callbacks DebugService::BuildSessionCallbacks(int session_id,
                                                            std::string session_label) {
  DebugSession::Callbacks callbacks;
  callbacks.on_output = [this, session_id, session_label](
                            const dap_protocol::DapOutputEvent& output) {
    util::InvokeIfSet(operations_.append_console_output, session_id, session_label, output);
    util::InvokeIfSet(operations_.request_frame);
  };
  callbacks.on_state_changed = [this](DebugSession::State state) {
    util::InvokeIfSet(operations_.notify_session_state_changed, state);
    SyncSessionsPanel();
    util::InvokeIfSet(operations_.request_frame);
  };
  callbacks.on_capabilities_changed = [this]() {
    util::InvokeIfSet(operations_.request_frame);
  };
  callbacks.on_terminated = [this, session_id](DebugSession::State terminal_state,
                                               const std::string& reason) {
    if (operations_.notify_session_terminated) {
      operations_.notify_session_terminated(session_id,
                                            terminal_state == DebugSession::State::Failed, reason);
    }
  };
  callbacks.on_stop_began = [this, session_id](const dap_protocol::DapStoppedEvent& stop) {
    if (IsActiveSession(session_id) && operations_.notify_stop_began) {
      operations_.notify_stop_began(stop.reason, stop.thread_id.value_or(0));
    }
  };
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
      manager.SetActiveSession(session_id);
      manager.SetSessionAttention(session_id, false);
      ClearTransientDebugViews();
      if (operations_.show_debug_console) {
        operations_.show_debug_console(session_id, manager.SessionLabel(session_id));
      }
      ProjectStop(stop, frames);
    }
    SyncSessionsPanel();
    util::InvokeIfSet(operations_.request_frame);
  };
  callbacks.on_resumed = [this, session_id]() {
    if (IsActiveSession(session_id)) {
      ClearTransientDebugViews();
    } else {
      CurrentDapManager().SetSessionAttention(session_id, false);
      SyncSessionsPanel();
      util::InvokeIfSet(operations_.request_frame);
    }
  };
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
    util::InvokeIfSet(operations_.request_frame);
  };
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
          if (i >= requested_lines.size()) {
            break;
          }
          results.push_back(editor::VerifiedBreakpoint{
              .id = breakpoint.id,
              .verified = breakpoint.verified,
              .line = requested_lines[i],
              .message = breakpoint.message,
          });
        }
        CurrentProjectState().breakpoint_store.ApplyVerification(path, results);
        SyncBreakpointsPanel();
        util::InvokeIfSet(operations_.request_frame);
      };
  callbacks.on_breakpoint_changed = [this](const std::filesystem::path& path,
                                           const dap_protocol::DapBreakpoint& breakpoint) {
    const bool claimed_by_function_bp =
        CurrentProjectState().function_breakpoint_store.ApplyBreakpointEvent(
            editor::VerifiedFunctionBreakpoint{
                .id = breakpoint.id,
                .verified = breakpoint.verified,
                .message = breakpoint.message,
            });
    if (!claimed_by_function_bp) {
      CurrentProjectState().breakpoint_store.ApplyBreakpointEvent(
          path, editor::VerifiedBreakpoint{
                    .id = breakpoint.id,
                    .verified = breakpoint.verified,
                    .line = breakpoint.line,
                    .message = breakpoint.message,
                });
    }
    SyncBreakpointsPanel();
    util::InvokeIfSet(operations_.request_frame);
  };
  callbacks.function_breakpoint_provider = [this]() {
    return CurrentProjectState().function_breakpoint_store.All();
  };
  callbacks.on_function_breakpoints_verified =
      [this](const std::vector<std::string>& requested_names,
             const std::vector<dap_protocol::DapBreakpoint>& breakpoints) {
        std::vector<editor::VerifiedFunctionBreakpoint> results;
        results.reserve(std::min(breakpoints.size(), requested_names.size()));
        for (std::size_t i = 0; i < breakpoints.size() && i < requested_names.size(); ++i) {
          results.push_back(editor::VerifiedFunctionBreakpoint{
              .id = breakpoints[i].id,
              .verified = breakpoints[i].verified,
              .message = breakpoints[i].message,
          });
        }
        CurrentProjectState().function_breakpoint_store.ApplyVerification(requested_names, results);
        SyncBreakpointsPanel();
        util::InvokeIfSet(operations_.request_frame);
      };
  callbacks.on_exception_filters_available =
      [this](const std::vector<dap_protocol::DapExceptionFilter>& filters) {
        CurrentProjectState().debug_breakpoints_panel.SetAdvertisedFilters(filters);
        SyncBreakpointsPanel();
        util::InvokeIfSet(operations_.request_frame);
      };
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
  frame_generation_.bump();
  watch_generation_.bump();
  ProjectWorkspaceState& state = CurrentProjectState();
  state.debug_execution.Clear();
  state.debug_variables.Clear();
  state.debug_hover.Clear();
  state.debug_watch.ClearResults();
  util::InvokeIfSet(operations_.request_frame);
}

}
