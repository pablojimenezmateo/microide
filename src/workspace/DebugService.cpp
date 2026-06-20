#include "workspace/DebugService.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

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
        if (operations_.show_debug_console) {
          const int id = manager.ActiveSessionId();
          operations_.show_debug_console(id, manager.SessionLabel(id));
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

// ProjectStop, BuildSessionCallbacks, and ClearTransientDebugViews (plus the
// BuildExecutionView / BuildThreadViews prebuilt-view helpers) live in
// DebugServiceCallbacks.cpp.

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
    ResetAdapterBreakpointState();
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
  ResetAdapterBreakpointState();
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
  const std::string label = manager.SessionLabel(session_id);
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

void DebugService::ResetAdapterBreakpointState() {
  // Verification state is tied to the adapter; drop it so a fresh session
  // re-verifies from scratch and the gutter shows unverified until then.
  CurrentProjectState().breakpoint_store.ResetVerification();
  CurrentProjectState().function_breakpoint_store.ResetVerification();
  // The advertised exception filters belong to the (now dead) adapter; drop them
  // so the Breakpoints tab shows only line breakpoints until a new session binds.
  CurrentProjectState().debug_breakpoints_panel.ClearAdvertisedFilters();
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

void DebugService::ReverseContinue() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->ReverseContinue();
  }
}

void DebugService::StepBack() {
  if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
    session->StepBack();
  }
}

bool DebugService::IsSessionActive() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session != nullptr && session->IsActive();
}

int DebugService::ActiveSessionId() const { return CurrentDapManager().ActiveSessionId(); }

std::string DebugService::ActiveSessionLabel() const {
  const DapManager& manager = CurrentDapManager();
  return manager.SessionLabel(manager.ActiveSessionId());
}

DebugSession::State DebugService::SessionState() const {
  const DebugSession* session = CurrentDapManager().ActiveSession();
  return session == nullptr ? DebugSession::State::Inactive : session->CurrentState();
}

std::string DebugService::LastError() const { return CurrentDapManager().LastError(); }

}  // namespace microide::workspace
