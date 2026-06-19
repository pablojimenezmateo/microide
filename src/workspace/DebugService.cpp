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
  // Prune sessions whose adapter has terminated and whose I/O thread has joined
  // (done outside DrainCallbacks so a session is never destroyed inside its own
  // callback). When the *active* session was the one removed, re-project the new
  // active session; a background prune leaves the active view untouched.
  const std::vector<int> removed = manager.PruneTerminated();
  if (!removed.empty()) {
    // Drop each pruned session's console channel + tab so dead consoles do not
    // accumulate (Phase 10). Done before re-projecting the survivor so the active
    // console switch below lands on a still-live channel.
    if (operations_.remove_debug_console) {
      for (const int id : removed) {
        operations_.remove_debug_console(id);
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
      [this](const std::filesystem::path& path,
             const std::vector<dap_protocol::DapBreakpoint>& breakpoints) {
        std::vector<editor::VerifiedBreakpoint> results;
        results.reserve(breakpoints.size());
        for (const dap_protocol::DapBreakpoint& breakpoint : breakpoints) {
          results.push_back(editor::VerifiedBreakpoint{
              .id = breakpoint.id,
              .verified = breakpoint.verified,
              .line = breakpoint.line,
              .message = breakpoint.message,
          });
        }
        CurrentProjectState().breakpoint_store.ApplyVerification(path, results);
        SyncBreakpointsPanel();
        if (operations_.request_editor_redraw) {
          operations_.request_editor_redraw();
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
  // The session pulls the enabled filter ids at `initialized` and on live re-send.
  callbacks.exception_filter_provider = [this]() {
    return CurrentProjectState().debug_breakpoints_panel.EnabledAdvertisedIds();
  };
  return callbacks;
}

void DebugService::ClearTransientDebugViews() {
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
    SyncSessionsPanel();
  }
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

void DebugService::ToggleExceptionFilter(const std::string& filter_id) {
  DebugBreakpointsModel& panel = CurrentProjectState().debug_breakpoints_panel;
  if (!panel.ToggleFilter(filter_id)) {
    return;
  }
  SyncBreakpointsPanel();
  // Live re-send so the change takes effect immediately on an active session.
  if (DebugSession* session = CurrentDapManager().ActiveSession();
      session != nullptr && session->IsActive()) {
    session->ResendExceptionFilters();
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::SyncBreakpointsPanel() {
  ProjectWorkspaceState& state = CurrentProjectState();
  state.debug_breakpoints_panel.Rebuild(state.breakpoint_store);
}

void DebugService::FocusFrame(int frame_id) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  // Hover values are frame-scoped: a frame switch must not serve a value (or let a
  // still-in-flight request resolve) keyed to the previously focused frame.
  util::DebugTrace::Note("locals", "focus-frame request-scopes frame",
                         static_cast<long long>(frame_id));
  CurrentProjectState().debug_hover.Clear();
  CurrentProjectState().debug_variables.BeginFrame(frame_id);
  session->RequestScopes(frame_id, [this](std::vector<dap_protocol::DapScope> scopes) {
    DebugVariablesModel& model = CurrentProjectState().debug_variables;
    // ApplyScopes re-expands the scopes the user had open before this stop and
    // returns the bounded fetches needed to repopulate them; issue each (children
    // that arrive cascade further) so a step does not collapse the tree.
    for (const DebugValueTree::ChildFetch& fetch : model.ApplyScopes(scopes)) {
      FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
    }
    // Scopes are installed collapsed; their variables are fetched lazily when the
    // user expands a row (ToggleVariableRow). We deliberately do NOT auto-expand
    // on every stop: a stop frequently lands where in-scope locals are not yet
    // constructed (function entry, `stopAtBeginningOfMainSubprogram`, any line
    // before a declaration), and formatting that uninitialized memory can make a
    // single-threaded adapter spin for an unbounded time (gdb's STL
    // pretty-printers loop on garbage container pointers). Because the adapter
    // serializes requests, that spin would block the *next* execution-control
    // request (continue/step/pause) — i.e. stepping would silently stop working.
    // Lazy expansion keeps the stop cheap and execution control responsive.
    if (operations_.request_debug_pane_redraw) {
      operations_.request_debug_pane_redraw();
    }
  });
  // Re-evaluate watch expressions in the (now focused) frame's scope. Runs on
  // every stop (top frame) and on a call-stack frame switch.
  EvaluateWatches(frame_id);
}

void DebugService::ToggleVariableRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_variables.ToggleRow(row);
  util::DebugTrace::Note("locals", "toggle-row ref", static_cast<long long>(fetch.reference),
                         static_cast<long long>(fetch.start));
  if (fetch.reference > 0) {
    FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::FetchVariablesPage(int reference, int start, int count) {
  if (reference <= 0) {
    return;
  }
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    // No live session to service the fetch: clear the loading state so the row
    // does not spin forever.
    CurrentProjectState().debug_variables.MarkChildrenError(reference);
    return;
  }
  session->RequestVariables(
      reference, start, count,
      [this, reference, start](bool ok, std::vector<dap_protocol::DapVariable> variables) {
        if (ok) {
          // Restoring expansion can cascade: applying a page may re-expand
          // descendants the user had open, whose own pages we fetch in turn.
          for (const DebugValueTree::ChildFetch& fetch :
               CurrentProjectState().debug_variables.ApplyVariables(reference, variables, start)) {
            FetchVariablesPage(fetch.reference, fetch.start, fetch.count);
          }
        } else {
          CurrentProjectState().debug_variables.MarkChildrenError(reference);
        }
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      });
}

void DebugService::BeginVariableEdit(std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  // Gate edit entry on the adapter capability so an unsupported adapter never
  // shows an edit field that cannot commit.
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (CurrentProjectState().debug_variables.BeginEdit(row) &&
      operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CommitVariableEdit() {
  DebugVariablesModel& model = CurrentProjectState().debug_variables;
  const auto target = model.EditTargetForCommit();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (target.has_value() && session != nullptr) {
    const std::string value = model.EditBuffer().text();
    const std::uint32_t node_id = target->node_id;
    session->SetVariable(
        target->container_reference, target->name, value,
        [this, node_id](bool ok, dap_protocol::DapSetVariableResult result) {
          // Authoritative: apply only the adapter's returned (possibly normalized)
          // value, never the raw typed text.
          if (ok) {
            CurrentProjectState().debug_variables.ApplySetVariable(node_id, result);
          }
          if (operations_.request_debug_pane_redraw) {
            operations_.request_debug_pane_redraw();
          }
        });
  }
  // Leave edit mode immediately; the row's value updates when the response lands.
  model.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CancelVariableEdit() {
  CurrentProjectState().debug_variables.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::AppendConsoleLine(int session_id, const std::string& label,
                                     const std::string& text) {
  if (!operations_.append_console_output) {
    return;
  }
  dap_protocol::DapOutputEvent event;
  event.category = "console";
  event.output = text + "\n";
  operations_.append_console_output(session_id, label, event);
}

bool DebugService::EvaluateRepl(const std::string& expression) {
  if (expression.empty()) {
    return false;
  }
  DapManager& manager = CurrentDapManager();
  DebugSession* session = manager.ActiveSession();
  if (session == nullptr) {
    return false;
  }
  const int session_id = manager.ActiveSessionId();
  std::string label;
  for (const DapSessionInfo& info : manager.Sessions()) {
    if (info.id == session_id) {
      label = info.name;
      break;
    }
  }
  // Echo the typed expression, then surface the console so the result is visible.
  AppendConsoleLine(session_id, label, "> " + expression);
  if (operations_.show_debug_console) {
    operations_.show_debug_console(session_id, label);
  }
  // Frame 0 when running (no stopped frame); the adapter evaluates in global scope.
  session->RequestEvaluate(
      expression, FocusedFrameId(), "repl",
      [this, session_id, label](bool ok, dap_protocol::DapEvaluateResult result) {
        if (!ok) {
          AppendConsoleLine(session_id, label, "error: could not evaluate expression");
          if (operations_.request_debug_pane_redraw) {
            operations_.request_debug_pane_redraw();
          }
          return;
        }
        std::string line = result.result.empty() ? std::string("(no value)") : result.result;
        if (!result.type.empty()) {
          line += "  : " + result.type;
        }
        AppendConsoleLine(session_id, label, line);
        // Structured result: expand one level of children inline as indented
        // `name: value` lines so a dict/object prints its fields (Phase 10). Deep
        // lazy expansion would need a tree surface; the console is text, so a single
        // eager level covers the common "print this object" case.
        if (result.variables_reference > 0) {
          if (DebugSession* child_session = CurrentDapManager().SessionById(session_id);
              child_session != nullptr) {
            child_session->RequestVariables(
                result.variables_reference, 0, DebugValueTree::kChildPageSize,
                [this, session_id, label](bool ok, std::vector<dap_protocol::DapVariable> variables) {
                  if (!ok) {
                    return;
                  }
                  for (const dap_protocol::DapVariable& variable : variables) {
                    AppendConsoleLine(session_id, label, "    " + variable.name + ": " +
                                                             variable.value);
                  }
                  if (operations_.request_debug_pane_redraw) {
                    operations_.request_debug_pane_redraw();
                  }
                });
          }
        }
        if (operations_.request_debug_pane_redraw) {
          operations_.request_debug_pane_redraw();
        }
      });
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
  return true;
}

int DebugService::FocusedFrameId() const {
  const DebugStackFrameView* frame = CurrentProjectState().debug_execution.FocusedFrame();
  // -1 (not 0) when no frame is focused: frame id 0 is gdb's valid top frame, so a
  // 0 here would wrongly request evaluation in a real frame. -1 means "no frame"
  // and MakeEvaluateArguments omits frameId (global-scope evaluate while running).
  return frame != nullptr ? frame->id : -1;
}

void DebugService::EvaluateWatches(int frame_id) {
  DebugWatchModel& watch = CurrentProjectState().debug_watch;
  // Rebuild one placeholder root per expression so rows stay stable/ordered while
  // the (async) results stream in by index.
  watch.BeginEvaluation();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session != nullptr) {
    const std::vector<std::string> expressions = watch.Expressions();
    for (std::size_t i = 0; i < expressions.size(); ++i) {
      session->RequestEvaluate(
          expressions[i], frame_id, "watch",
          [this, i](bool ok, dap_protocol::DapEvaluateResult result) {
            if (ok) {
              CurrentProjectState().debug_watch.ApplyEvaluate(i, result);
            }
            if (operations_.request_debug_pane_redraw) {
              operations_.request_debug_pane_redraw();
            }
          });
    }
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

std::size_t DebugService::AddWatch(std::string expression) {
  const std::size_t index = CurrentProjectState().debug_watch.AddExpression(std::move(expression));
  EvaluateWatches(FocusedFrameId());
  return index;
}

void DebugService::EditWatch(std::size_t index, std::string expression) {
  CurrentProjectState().debug_watch.EditExpression(index, std::move(expression));
  EvaluateWatches(FocusedFrameId());
}

void DebugService::RemoveWatch(std::size_t index) {
  CurrentProjectState().debug_watch.RemoveExpression(index);
  EvaluateWatches(FocusedFrameId());
}

void DebugService::ToggleWatchRow(std::size_t row) {
  const DebugValueTree::ChildFetch fetch = CurrentProjectState().debug_watch.ToggleRow(row);
  if (fetch.reference > 0) {
    DebugSession* session = CurrentDapManager().ActiveSession();
    if (session != nullptr) {
      const int reference = fetch.reference;
      const int start = fetch.start;
      session->RequestVariables(
          reference, start, fetch.count,
          [this, reference, start](bool ok, std::vector<dap_protocol::DapVariable> variables) {
            if (ok) {
              CurrentProjectState().debug_watch.ApplyVariables(reference, variables, start);
            } else {
              CurrentProjectState().debug_watch.MarkChildrenError(reference);
            }
            if (operations_.request_debug_pane_redraw) {
              operations_.request_debug_pane_redraw();
            }
          });
    } else {
      CurrentProjectState().debug_watch.MarkChildrenError(fetch.reference);
    }
  }
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::BeginWatchEdit(std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (CurrentProjectState().debug_watch.BeginEdit(row) && operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CommitWatchEdit() {
  DebugWatchModel& model = CurrentProjectState().debug_watch;
  const auto target = model.EditTargetForCommit();
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (target.has_value() && session != nullptr) {
    const std::string value = model.EditBuffer().text();
    const std::uint32_t node_id = target->node_id;
    session->SetVariable(target->container_reference, target->name, value,
                         [this, node_id](bool ok, dap_protocol::DapSetVariableResult result) {
                           if (ok) {
                             CurrentProjectState().debug_watch.ApplySetVariable(node_id, result);
                           }
                           if (operations_.request_debug_pane_redraw) {
                             operations_.request_debug_pane_redraw();
                           }
                         });
  }
  model.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::CancelWatchEdit() {
  CurrentProjectState().debug_watch.CancelEdit();
  if (operations_.request_debug_pane_redraw) {
    operations_.request_debug_pane_redraw();
  }
}

void DebugService::EvaluateHover(int frame_id, const std::string& expression) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  DebugHoverModel& hover = CurrentProjectState().debug_hover;
  // Dedup: the per-frame hover trigger re-asks every mouse-move; only the first
  // ask for a given (frame, expression) issues a request. Pending/Resolved/Failed
  // for the same key are all served (or suppressed) without re-issuing.
  if (hover.Classify(frame_id, expression) != DebugHoverModel::Lookup::Miss) {
    util::DebugTrace::Note("hover", "evaluate suppressed (not a miss) expr", expression,
                           static_cast<long long>(frame_id));
    return;
  }
  util::DebugTrace::Note("hover", "evaluate begin expr", expression,
                         static_cast<long long>(frame_id));
  const std::uint64_t generation = hover.Begin(frame_id, expression);
  session->RequestEvaluate(
      expression, frame_id, "hover",
      [this, generation](bool ok, dap_protocol::DapEvaluateResult result) {
        DebugHoverModel& model = CurrentProjectState().debug_hover;
        if (ok) {
          util::DebugTrace::Note("hover", "evaluate resolved value", result.result);
          model.Resolve(generation, std::move(result.result), std::move(result.type));
        } else {
          util::DebugTrace::Note("hover", "evaluate FAILED");
          model.Fail(generation);
        }
        // Queue a hover refresh first so the redraw re-resolves the now-cached
        // value into an active popup (mirrors ClearDiagnosticsForPath). Without
        // this the value sits in the cache and the tooltip never appears.
        if (operations_.queue_editor_hover_refresh) {
          operations_.queue_editor_hover_refresh();
        }
        if (operations_.request_editor_redraw) {
          operations_.request_editor_redraw();
        }
      });
}

bool DebugService::SupportsEvaluateForHovers() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->Client().Capabilities().supports_evaluate_for_hovers;
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

DebugSession::State DebugService::SessionState() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session == nullptr ? DebugSession::State::Inactive : session->CurrentState();
}

std::string DebugService::LastError() const { return CurrentDapManager().LastError(); }

}  // namespace microide::workspace
