#include "workspace/DebugService.h"

#include <algorithm>
#include <cstdint>
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
    if (operations_.request_bottom_panel_redraw) {
      operations_.request_bottom_panel_redraw();
    }
  };
  // On resume: drop the execution view + variables so the highlight, stack, and
  // variables tree clear at once.
  callbacks.on_resumed = [this]() {
    CurrentProjectState().debug_execution.Clear();
    CurrentProjectState().debug_variables.Clear();
    CurrentProjectState().debug_hover.Clear();
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
  // Drop the execution view + variables so the highlight, Call Stack, and
  // Variables panels clear.
  CurrentProjectState().debug_execution.Clear();
  CurrentProjectState().debug_variables.Clear();
  CurrentProjectState().debug_hover.Clear();
  if (operations_.request_editor_redraw) {
    operations_.request_editor_redraw();
  }
  if (operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
  }
}

void DebugService::FocusFrame(int frame_id) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  if (session == nullptr) {
    return;
  }
  // Hover values are frame-scoped: a frame switch must not serve a value (or let a
  // still-in-flight request resolve) keyed to the previously focused frame.
  CurrentProjectState().debug_hover.Clear();
  CurrentProjectState().debug_variables.BeginFrame(frame_id);
  session->RequestScopes(frame_id, [this](std::vector<dap_protocol::DapScope> scopes) {
    DebugVariablesModel& model = CurrentProjectState().debug_variables;
    model.ApplyScopes(scopes);
    // Auto-expand the first scope (conventionally "Locals") for immediate
    // visibility; this issues one `variables` request via ToggleVariableRow.
    if (!model.Rows().empty()) {
      ToggleVariableRow(0);
    }
    if (operations_.request_bottom_panel_redraw) {
      operations_.request_bottom_panel_redraw();
    }
  });
}

void DebugService::ToggleVariableRow(std::size_t row) {
  const int reference = CurrentProjectState().debug_variables.ToggleRow(row);
  if (reference > 0) {
    if (DebugSession* session = CurrentDapManager().ActiveSession(); session != nullptr) {
      session->RequestVariables(
          reference, [this, reference](std::vector<dap_protocol::DapVariable> variables) {
            CurrentProjectState().debug_variables.ApplyVariables(reference, variables);
            if (operations_.request_bottom_panel_redraw) {
              operations_.request_bottom_panel_redraw();
            }
          });
    }
  }
  if (operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
  }
}

void DebugService::BeginVariableEdit(std::size_t row) {
  DebugSession* session = CurrentDapManager().ActiveSession();
  // Gate edit entry on the adapter capability so an unsupported adapter never
  // shows an edit field that cannot commit.
  if (session == nullptr || !session->Client().Capabilities().supports_set_variable) {
    return;
  }
  if (CurrentProjectState().debug_variables.BeginEdit(row) &&
      operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
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
          if (operations_.request_bottom_panel_redraw) {
            operations_.request_bottom_panel_redraw();
          }
        });
  }
  // Leave edit mode immediately; the row's value updates when the response lands.
  model.CancelEdit();
  if (operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
  }
}

void DebugService::CancelVariableEdit() {
  CurrentProjectState().debug_variables.CancelEdit();
  if (operations_.request_bottom_panel_redraw) {
    operations_.request_bottom_panel_redraw();
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
    return;
  }
  const std::uint64_t generation = hover.Begin(frame_id, expression);
  session->RequestEvaluate(
      expression, frame_id, "hover",
      [this, generation](bool ok, dap_protocol::DapEvaluateResult result) {
        DebugHoverModel& model = CurrentProjectState().debug_hover;
        if (ok) {
          model.Resolve(generation, std::move(result.result), std::move(result.type));
        } else {
          model.Fail(generation);
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
