#include "workspace/DebugService.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/DebugTrace.h"
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

void DebugService::Configure(WorkspaceContext& context, Operations operations) {
  context_ = &context;
  operations_ = std::move(operations);
}

void DebugService::SetWakeEventType(Uint32 event_type) {
  wake_event_type_ = event_type;
  if (context_ != nullptr) {
    EnsureProjectDapManager(CurrentProjectState()).SetWakeEventType(event_type);
  }
}

ProjectWorkspaceState& DebugService::CurrentProjectState() {
  return context_->current_project_state;
}

const ProjectWorkspaceState& DebugService::CurrentProjectState() const {
  return context_->current_project_state;
}

DapManager& DebugService::EnsureProjectDapManager(ProjectWorkspaceState& state) {
  if (state.dap_manager == nullptr) {
    state.dap_manager = std::make_unique<DapManager>();
  }
  if (wake_event_type_ != 0) {
    state.dap_manager->SetWakeEventType(wake_event_type_);
  }
  return *state.dap_manager;
}

DapManager& DebugService::CurrentDapManager() {
  return EnsureProjectDapManager(CurrentProjectState());
}

const DapManager& DebugService::CurrentDapManager() const {
  return const_cast<DebugService*>(this)->CurrentDapManager();
}

void DebugService::ConsumeDapCallbacks() {
  DapManager& manager = CurrentDapManager();
  const int active_before = manager.ActiveSessionId();
  manager.DrainCallbacks();
  // Reconcile any session whose adapter process died without a DAP terminated/
  // exited event (crash / external kill / RLIMIT_AS cap) so it transitions terminal
  // and can be pruned below instead of lingering as a zombie row. Done after
  // DrainCallbacks (so a real terminated event wins) and before PruneTerminated.
  // A reconciled session is always dead (IsRunning() already false), so the prune
  // below removes it the same frame; its row also re-syncs via on_state_changed.
  manager.ReapExitedSessions();
  // Prune sessions whose adapter has terminated and whose I/O thread has joined
  // (done outside DrainCallbacks so a session is never destroyed inside its own
  // callback). When the *active* session was the one removed, re-project the new
  // active session; a background prune leaves the active view untouched.
  const std::vector<PrunedSession> removed = manager.PruneTerminated();
  if (!removed.empty()) {
    // Settle each pruned session's console. A clean exit drops its channel + tab so
    // dead consoles do not accumulate (Phase 10). A non-clean end (crash / kill /
    // launch rejection) keeps its console so the adapter's output stays inspectable,
    // and appends why it ended — otherwise the session would vanish silently. Done
    // before re-projecting the survivor so the active console switch below lands on
    // a still-live channel.
    for (const PrunedSession& pruned : removed) {
      if (pruned.failed) {
        if (!pruned.error.empty()) {
          AppendConsoleLine(pruned.id, pruned.console_label, "[debug] " + pruned.error);
        }
      } else if (operations_.remove_debug_console) {
        operations_.remove_debug_console(pruned.id);
      }
    }
    if (manager.ActiveSessionId() != active_before) {
      ClearTransientDebugViews();
      if (DebugSession* active = manager.ActiveSession(); active != nullptr) {
        for (const DapSessionInfo& info : manager.Sessions()) {
          if (info.id == manager.ActiveSessionId() && operations_.show_debug_console) {
            operations_.show_debug_console(info.id, info.name);
            break;
          }
        }
        active->Reactivate();
      }
    }
    SyncSessionsPanel();
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  }
}

bool DebugService::IsActiveSession(int session_id) const {
  return CurrentDapManager().ActiveSessionId() == session_id;
}

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
      for (const DapSessionInfo& info : manager.Sessions()) {
        if (info.id == session_id && operations_.show_debug_console) {
          operations_.show_debug_console(info.id, info.name);
          break;
        }
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
  ++frame_generation_;
  ++watch_generation_;
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

int DebugService::LaunchSession(const LaunchConfig& config, const std::string& cwd, bool replace) {
  last_launch_config_ = config;
  last_cwd_ = cwd;
  // A new session: forget the prior session's variable expansion and re-arm the
  // Locals-open-by-default one-shot so Locals opens on this session's first stop.
  CurrentProjectState().debug_variables.BeginSession();
  const std::string label = !config.name.empty() ? config.name : config.type;
  DapManager& manager = CurrentDapManager();
  auto factory = [this, label](int id) { return BuildSessionCallbacks(id, label); };
  const int id =
      replace ? manager.ReplaceActiveSession(config, factory, cwd)
              : manager.StartSession(config, factory, cwd);
  if (id != 0) {
    if (operations_.show_debug_console) {
      operations_.show_debug_console(id, label);
    }
  }
  // Re-sync unconditionally: on spawn failure the failing session fired a
  // synchronous Failed state-change (and a SyncSessionsPanel) while it was still in
  // the manager's list, then was popped — so the panel holds a phantom row until we
  // rebuild from the now-current session set.
  SyncSessionsPanel();
  return id;
}

bool DebugService::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  return LaunchSession(config, cwd, /*replace=*/false) != 0;
}

void DebugService::StopDebugging() {
  DapManager& manager = CurrentDapManager();
  // Stopping the last live session resets the project-shared adapter state
  // (breakpoint verification + advertised exception filters). With other sessions
  // still live, leave that shared state intact — they still rely on it.
  const bool last_session = manager.SessionCount() <= 1;
  manager.StopActiveSession();
  if (last_session) {
    // Verification state is tied to the adapter; drop it so a fresh session
    // re-verifies from scratch and the gutter shows unverified until then.
    CurrentProjectState().breakpoint_store.ResetVerification();
    CurrentProjectState().function_breakpoint_store.ResetVerification();
    // The advertised exception filters belong to the (now dead) adapter; drop them
    // so the Breakpoints tab shows only line breakpoints until a new session binds.
    CurrentProjectState().debug_breakpoints_panel.ClearAdvertisedFilters();
  }
  SyncBreakpointsPanel();
  // Drop the execution view + variables; the next-frame prune advances the active
  // session and re-projects it when one remains.
  ClearTransientDebugViews();
  SyncSessionsPanel();
}

void DebugService::StopAllDebugging() {
  DapManager& manager = CurrentDapManager();
  if (manager.SessionCount() == 0) {
    return;
  }
  // Tear every session down (blocks until all adapter I/O threads join). The
  // next-frame prune drops the now-terminal sessions and cleans their consoles.
  manager.BeginShutdownAll();
  manager.ShutdownAll();
  // All sessions are gone, so reset the project-shared adapter state (same as
  // StopDebugging's last-session branch).
  CurrentProjectState().breakpoint_store.ResetVerification();
  CurrentProjectState().function_breakpoint_store.ResetVerification();
  CurrentProjectState().debug_breakpoints_panel.ClearAdvertisedFilters();
  SyncBreakpointsPanel();
  ClearTransientDebugViews();
  SyncSessionsPanel();
}

void DebugService::Restart() {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr || !session->IsActive()) {
    return;
  }
  if (session->Client().Capabilities().supports_restart_request) {
    // In-place restart: the adapter relaunches the debuggee and re-emits its
    // initialized/stopped sequence. Clear the current stop's transient views.
    session->Restart();
    ClearTransientDebugViews();
    return;
  }
  // Fallback: terminate + relaunch with the same config, replacing the active
  // session in place so no second session row appears. ReplaceActiveSession blocks
  // on the prior session's shutdown, so this is a clean synchronous relaunch.
  CurrentProjectState().breakpoint_store.ResetVerification();
  CurrentProjectState().function_breakpoint_store.ResetVerification();
  ClearTransientDebugViews();
  LaunchSession(last_launch_config_, last_cwd_, /*replace=*/true);
}

void DebugService::FocusSession(int session_id) {
  DapManager& manager = CurrentDapManager();
  DebugSession* session = manager.SessionById(session_id);
  if (session == nullptr) {
    return;
  }
  manager.SetActiveSession(session_id);
  manager.SetSessionAttention(session_id, false);
  // Drop the previously-active session's projection, then re-project this one.
  ClearTransientDebugViews();
  const std::string label = !session->Config().name.empty() ? session->Config().name
                                                            : session->Config().type;
  if (operations_.show_debug_console) {
    operations_.show_debug_console(session_id, label);
  }
  SyncSessionsPanel();
  // Re-project the picked session's current stop (no-op if it is running): the
  // stack/threads re-resolve and re-fire on_stopped through the active path.
  session->Reactivate();
}

void DebugService::FocusNextSession() {
  DapManager& manager = CurrentDapManager();
  const std::vector<DapSessionInfo> sessions = manager.Sessions();
  if (sessions.size() < 2) {
    return;
  }
  const int active = manager.ActiveSessionId();
  std::size_t index = 0;
  for (std::size_t i = 0; i < sessions.size(); ++i) {
    if (sessions[i].id == active) {
      index = i;
      break;
    }
  }
  FocusSession(sessions[(index + 1) % sessions.size()].id);
}

std::vector<DapSessionInfo> DebugService::Sessions() const {
  return CurrentDapManager().Sessions();
}

void DebugService::SyncSessionsPanel() {
  DapManager& manager = CurrentDapManager();
  DebugExecutionView& view = CurrentProjectState().debug_execution;
  view.sessions.clear();
  for (const DapSessionInfo& info : manager.Sessions()) {
    DebugSessionView row;
    row.id = info.id;
    row.attention = info.attention;
    std::string display = info.name;
    const char* word = nullptr;
    switch (info.state) {
      case DebugSession::State::Initializing:
      case DebugSession::State::Configuring:
        word = "starting";
        break;
      case DebugSession::State::Running:
        word = "running";
        break;
      case DebugSession::State::Stopped:
        word = "paused";
        break;
      case DebugSession::State::Terminated:
        word = "terminated";
        break;
      case DebugSession::State::Failed:
        word = "failed";
        break;
      case DebugSession::State::Inactive:
        break;
    }
    if (word != nullptr) {
      display += " (";
      display += word;
      display += ')';
    }
    row.display = std::move(display);
    view.sessions.push_back(std::move(row));
  }
  view.focused_session_id = manager.ActiveSessionId();
}

void DebugService::FocusThread(int thread_id) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  CurrentProjectState().debug_execution.focused_thread_id = thread_id;
  session->SwitchThread(thread_id);
}

bool DebugService::HasInFlightDapWork() const {
  // Read the manager directly (no EnsureProjectDapManager) so this query, called
  // from the idle loop, has no side effects.
  const ProjectWorkspaceState& state = CurrentProjectState();
  if (state.dap_manager == nullptr) {
    return false;
  }
  const DebugSession* session = state.dap_manager->ActiveSession();
  return session != nullptr && session->IsActive() && session->Client().HasPendingRequests();
}

void DebugService::ResendBreakpointsForFile(const std::filesystem::path& path) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session != nullptr && session->IsActive()) {
    session->ResendBreakpointsForFile(path);
  }
}

void DebugService::Continue() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->Continue();
  }
}

void DebugService::StepOver() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->StepOver();
  }
}

void DebugService::StepIn() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->StepIn();
  }
}

void DebugService::StepOut() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->StepOut();
  }
}

void DebugService::Pause() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->Pause();
  }
}

bool DebugService::IsSessionActive() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->IsActive();
}

int DebugService::ActiveSessionId() const { return CurrentDapManager().ActiveSessionId(); }

std::string DebugService::ActiveSessionLabel() const {
  const int id = ActiveSessionId();
  for (const DapSessionInfo& info : Sessions()) {
    if (info.id == id) {
      return info.name;
    }
  }
  return {};
}

DebugSession::State DebugService::SessionState() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session == nullptr ? DebugSession::State::Inactive : session->CurrentState();
}

std::string DebugService::LastError() const { return CurrentDapManager().LastError(); }

}  // namespace microide::workspace
