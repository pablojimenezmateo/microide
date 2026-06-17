#include "workspace/DebugService.h"

#include <utility>

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

void DebugService::ConsumeDapCallbacks() { CurrentDapManager().DrainCallbacks(); }

bool DebugService::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  DebugSession::Callbacks callbacks;
  callbacks.on_output = [this](const dap_protocol::DapOutputEvent& output) {
    if (operations_.append_console_output) {
      operations_.append_console_output(output);
    }
    if (operations_.request_bottom_panel_redraw) {
      operations_.request_bottom_panel_redraw();
    }
  };
  callbacks.on_state_changed = [this](DebugSession::State state) {
    if (operations_.notify_session_state_changed) {
      operations_.notify_session_state_changed(state);
    }
    if (operations_.request_chrome_redraw) {
      operations_.request_chrome_redraw();
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
        if (operations_.request_editor_redraw) {
          operations_.request_editor_redraw();
        }
      };
  return CurrentDapManager().StartSession(config, std::move(callbacks), cwd);
}

void DebugService::StopDebugging() {
  CurrentDapManager().StopActiveSession();
  // Verification state is tied to the adapter; drop it so a fresh session
  // re-verifies from scratch and the gutter shows unverified until then.
  CurrentProjectState().breakpoint_store.ResetVerification();
  if (operations_.request_editor_redraw) {
    operations_.request_editor_redraw();
  }
}

void DebugService::ResendBreakpointsForFile(const std::filesystem::path& path) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session != nullptr && session->IsActive()) {
    session->ResendBreakpointsForFile(path);
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
