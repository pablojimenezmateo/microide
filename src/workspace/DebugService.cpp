#include "workspace/DebugService.h"

#include <algorithm>
#include <string>
#include <utility>

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
  // On every stop: rebuild the execution view (call stack + focused frame),
  // surface the Call Stack panel, and jump the editor to the top frame.
  callbacks.on_stopped = [this](const dap_protocol::DapStoppedEvent& stop,
                                const std::vector<dap_protocol::DapStackFrame>& frames) {
    ProjectWorkspaceState& state = CurrentProjectState();
    state.debug_execution = BuildExecutionView(stop, frames);
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
    if (operations_.request_bottom_panel_redraw) {
      operations_.request_bottom_panel_redraw();
    }
  };
  // On resume: drop the execution view so the highlight + stack clear at once.
  callbacks.on_resumed = [this]() {
    CurrentProjectState().debug_execution.Clear();
    if (operations_.request_editor_redraw) {
      operations_.request_editor_redraw();
    }
    if (operations_.request_bottom_panel_redraw) {
      operations_.request_bottom_panel_redraw();
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
  // Drop the execution view so the highlight + Call Stack panel clear.
  CurrentProjectState().debug_execution.Clear();
  if (operations_.request_editor_redraw) {
    operations_.request_editor_redraw();
  }
  if (operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
  }
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
