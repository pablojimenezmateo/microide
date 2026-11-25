#include "workspace/debug/DebugService.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

void DebugService::Configure(WorkspaceContext& context, Operations operations) {
  context_ = &context;
  operations_ = std::move(operations);
}

void DebugService::SetWakeChannel(Uint32 event_type) {
  wake_event_type_ = event_type;
  if (context_ != nullptr) {
    EnsureProjectDapManager(CurrentProjectState()).SetWakeChannel(event_type);
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
    state.dap_manager->SetWakeChannel(wake_event_type_);
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
  manager.ReapExitedSessions();
  const std::vector<PrunedSession> removed = manager.PruneTerminated();
  if (!removed.empty()) {
    for (const PrunedSession& pruned : removed) {
      if (pruned.failed) {
        if (!pruned.error.empty()) {
          AppendConsoleLine(pruned.id, pruned.console_label, "[debug] " + pruned.error);
        }
      } else {
        AppendConsoleLine(pruned.id, pruned.console_label, "[debug] session ended");
        ended_console_ids_.push_back(pruned.id);
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
    util::InvokeIfSet(operations_.request_debug_pane_redraw);
  }
}

bool DebugService::IsActiveSession(int session_id) const {
  return CurrentDapManager().ActiveSessionId() == session_id;
}

int DebugService::LaunchSession(const LaunchConfig& unexpanded, const std::string& cwd, bool replace) {
  LaunchConfig config = unexpanded;
  ExpandLaunchVariables(config.arguments, cwd);
  last_launch_config_ = config;
  last_cwd_ = cwd;
  CurrentProjectState().debug_variables.BeginSession();
  if (operations_.remove_debug_console) {
    for (const int ended : ended_console_ids_) {
      operations_.remove_debug_console(ended);
    }
  }
  ended_console_ids_.clear();
  const std::string label = !config.name.empty() ? config.name : config.type;
  DapManager& manager = CurrentDapManager();
  auto factory = [this, label](int id) { return BuildSessionCallbacks(id, label); };
  const int id =
      replace ? manager.ReplaceActiveSession(config, factory, cwd)
              : manager.StartSession(config, factory, cwd);
  if (id != 0) {
    util::InvokeIfSet(operations_.show_debug_console, id, label);
  }
  SyncSessionsPanel();
  return id;
}

bool DebugService::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  return LaunchSession(config, cwd, false) != 0;
}

void DebugService::StopDebugging() {
  DapManager& manager = CurrentDapManager();
  const bool last_session = manager.SessionCount() <= 1;
  manager.StopActiveSession();
  if (last_session) {
    ResetAdapterBreakpointState();
  }
  SyncBreakpointsPanel();
  ClearTransientDebugViews();
  SyncSessionsPanel();
}

void DebugService::StopAllDebugging() {
  DapManager& manager = CurrentDapManager();
  if (manager.SessionCount() == 0) {
    return;
  }
  manager.BeginShutdownAll();
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
    session->Restart();
    ClearTransientDebugViews();
    return;
  }
  CurrentProjectState().breakpoint_store.ResetVerification();
  CurrentProjectState().function_breakpoint_store.ResetVerification();
  ClearTransientDebugViews();
  LaunchSession(last_launch_config_, last_cwd_, true);
}

void DebugService::FocusSession(int session_id) {
  DapManager& manager = CurrentDapManager();
  DebugSession* session = manager.SessionById(session_id);
  if (session == nullptr) {
    return;
  }
  manager.SetActiveSession(session_id);
  manager.SetSessionAttention(session_id, false);
  ClearTransientDebugViews();
  const std::string label = manager.SessionLabel(session_id);
  util::InvokeIfSet(operations_.show_debug_console, session_id, label);
  SyncSessionsPanel();
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

std::size_t DebugService::SessionCount() const { return CurrentDapManager().SessionCount(); }

void DebugService::SwitchSession(int index) {
  if (index < 0) {
    FocusNextSession();
    return;
  }
  const std::vector<DapSessionInfo> sessions = Sessions();
  if (index >= 1 && static_cast<std::size_t>(index) <= sessions.size()) {
    FocusSession(sessions[static_cast<std::size_t>(index - 1)].id);
  }
}

void DebugService::ResetAdapterBreakpointState() {
  CurrentProjectState().breakpoint_store.ResetVerification();
  CurrentProjectState().function_breakpoint_store.ResetVerification();
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
  SyncBreakpointsPanel();
  util::InvokeIfSet(operations_.request_debug_pane_redraw);
  util::InvokeIfSet(operations_.request_editor_redraw);
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

}
