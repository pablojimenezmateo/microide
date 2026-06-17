#include "workspace/DebugSession.h"

#include <utility>

#include "util/JsonValue.h"

namespace microide::workspace {

namespace {

util::JsonValue ThreadIdArgs(int thread_id) {
  util::JsonObject args;
  args["threadId"] = util::JsonValue(static_cast<std::int64_t>(thread_id));
  return util::JsonValue(std::move(args));
}

bool IsTerminalState(DebugSession::State state) {
  return state == DebugSession::State::Terminated || state == DebugSession::State::Failed;
}

}  // namespace

const char* DebugSessionStateName(DebugSession::State state) {
  switch (state) {
    case DebugSession::State::Inactive:
      return "inactive";
    case DebugSession::State::Initializing:
      return "initializing";
    case DebugSession::State::Configuring:
      return "configuring";
    case DebugSession::State::Running:
      return "running";
    case DebugSession::State::Stopped:
      return "stopped";
    case DebugSession::State::Terminated:
      return "terminated";
    case DebugSession::State::Failed:
      return "failed";
  }
  return "unknown";
}

DebugSession::DebugSession() : client_(std::make_unique<DapClient>()) {}

DebugSession::DebugSession(std::unique_ptr<DapClient> client) : client_(std::move(client)) {
  if (client_ == nullptr) {
    client_ = std::make_unique<DapClient>();
  }
}

DebugSession::~DebugSession() = default;

void DebugSession::SetWakeEventType(Uint32 event_type) { client_->SetWakeEventType(event_type); }

void DebugSession::SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

bool DebugSession::IsActive() const {
  switch (state_) {
    case State::Initializing:
    case State::Configuring:
    case State::Running:
    case State::Stopped:
      return true;
    case State::Inactive:
    case State::Terminated:
    case State::Failed:
      return false;
  }
  return false;
}

bool DebugSession::Start(const std::vector<std::string>& command, const LaunchConfig& config,
                         const std::string& cwd, const platform::SubprocessSandbox& sandbox) {
  config_ = config;
  launch_sent_ = false;
  configuration_done_sent_ = false;
  last_error_.clear();

  client_->SetEventCallback(
      [this](const std::string& event, const util::JsonValue& body) { HandleEvent(event, body); });

  if (!client_->Start(command, config_.type, cwd, sandbox)) {
    last_error_ = client_->LastError();
    if (last_error_.empty()) {
      last_error_ = "debug adapter failed to start";
    }
    SetState(State::Failed);
    return false;
  }

  SetState(State::Initializing);
  // The launch/attach request is queued now; the client flushes it once the
  // `initialize` response arrives (DapClient defers pre-initialize requests).
  SendLaunchRequest();
  return true;
}

void DebugSession::SendLaunchRequest() {
  if (launch_sent_) {
    return;
  }
  launch_sent_ = true;
  const std::string verb = config_.IsAttach() ? "attach" : "launch";
  client_->SendRequestAsync(verb, config_.arguments,
                            [this, verb](const dap_protocol::DapResponse& response) {
                              if (!response.success) {
                                last_error_ = response.message.empty()
                                                  ? (verb + " request was rejected")
                                                  : response.message;
                                SetState(State::Failed);
                                client_->BeginShutdown();
                                return;
                              }
                              // Per the DAP handshake the launch response usually
                              // arrives after configurationDone, but some adapters
                              // need no configuration phase — converge to Running.
                              if (state_ == State::Initializing || state_ == State::Configuring) {
                                SetState(State::Running);
                              }
                            });
}

void DebugSession::SendConfigurationDone() {
  if (configuration_done_sent_) {
    return;
  }
  configuration_done_sent_ = true;

  if (!client_->Capabilities().supports_configuration_done_request) {
    // No configuration phase: the adapter is ready to run once launched.
    if (state_ == State::Initializing) {
      SetState(State::Running);
    }
    return;
  }

  if (state_ == State::Initializing) {
    SetState(State::Configuring);
  }
  client_->SendRequestAsync("configurationDone", util::JsonValue(nullptr),
                            [this](const dap_protocol::DapResponse& response) {
                              if (!response.success && last_error_.empty()) {
                                last_error_ = response.message.empty()
                                                  ? "configurationDone was rejected"
                                                  : response.message;
                              }
                              if (state_ == State::Configuring || state_ == State::Initializing) {
                                SetState(State::Running);
                              }
                            });
}

void DebugSession::SendAllBreakpoints() {
  if (!callbacks_.breakpoint_provider) {
    return;
  }
  for (const auto& file : callbacks_.breakpoint_provider()) {
    SendBreakpointsForFile(file);
  }
}

void DebugSession::SendBreakpointsForFile(const editor::BreakpointStore::FileBreakpoints& file) {
  const std::string source_path = file.path.generic_string();
  if (source_path.empty()) {
    return;
  }
  const dap_protocol::DapCapabilities& caps = client_->Capabilities();
  std::vector<dap_protocol::SetBreakpointInput> inputs;
  inputs.reserve(file.breakpoints.size());
  for (const editor::Breakpoint& breakpoint : file.breakpoints) {
    if (!breakpoint.enabled) {
      continue;
    }
    dap_protocol::SetBreakpointInput input;
    // BreakpointStore stores 0-based buffer lines; DAP wants 1-based.
    input.line = static_cast<int>(breakpoint.line) + 1;
    // Phase 6 fields, gated on adapter capabilities so we never send a key an
    // adapter rejects. Empty values are omitted by the encoder regardless.
    if (caps.supports_conditional_breakpoints && breakpoint.condition) {
      input.condition = *breakpoint.condition;
    }
    if (caps.supports_hit_conditional_breakpoints && breakpoint.hit_condition) {
      input.hit_condition = *breakpoint.hit_condition;
    }
    if (caps.supports_log_points && breakpoint.log_message) {
      input.log_message = *breakpoint.log_message;
    }
    inputs.push_back(std::move(input));
  }

  const std::filesystem::path path = file.path;
  client_->SendRequestAsync(
      "setBreakpoints", dap_protocol::MakeSetBreakpointsArguments(source_path, inputs),
      [this, path](const dap_protocol::DapResponse& response) {
        if (response.success && callbacks_.on_breakpoints_verified) {
          callbacks_.on_breakpoints_verified(path, dap_protocol::ParseBreakpoints(response.body));
        }
      });
}

void DebugSession::ResendBreakpointsForFile(const std::filesystem::path& path) {
  if (!client_->IsInitialized() || !callbacks_.breakpoint_provider) {
    return;
  }
  const std::string target_key = path.lexically_normal().generic_string();
  for (const auto& file : callbacks_.breakpoint_provider()) {
    if (file.path.lexically_normal().generic_string() == target_key) {
      SendBreakpointsForFile(file);
      return;
    }
  }
  // No breakpoints remain for the file: send an empty list to clear them.
  SendBreakpointsForFile(editor::BreakpointStore::FileBreakpoints{.path = path});
}

void DebugSession::HandleEvent(const std::string& event, const util::JsonValue& body) {
  if (event == "initialized") {
    // The adapter is ready for configuration. Install breakpoints first, then
    // finalize with configurationDone. Both ride the client's single ordered
    // stream, so configurationDone reaches the adapter after every
    // setBreakpoints request (the DAP handshake requires send-order, not
    // response-order); verification reflects back asynchronously.
    SendAllBreakpoints();
    SendConfigurationDone();
  } else if (event == "output") {
    if (callbacks_.on_output) {
      callbacks_.on_output(dap_protocol::ParseOutputEvent(body));
    }
  } else if (event == "terminated" || event == "exited") {
    if (state_ != State::Terminated) {
      SetState(State::Terminated);
      client_->BeginShutdown();
    }
  } else if (event == "stopped") {
    const dap_protocol::DapStoppedEvent stop = dap_protocol::ParseStoppedEvent(body);
    stopped_thread_id_ = stop.thread_id;
    SetState(State::Stopped);
    // Resolve the call stack for the stopped thread, then hand the focused
    // frames up so the host can highlight the execution line + fill the panel.
    RequestStackTrace(stop);
  } else if (event == "continued") {
    if (state_ == State::Stopped) {
      if (callbacks_.on_resumed) {
        callbacks_.on_resumed();
      }
      SetState(State::Running);
    }
  }

  if (callbacks_.on_event) {
    callbacks_.on_event(event, body);
  }
}

void DebugSession::RequestStop() {
  if (state_ == State::Inactive || IsTerminalState(state_)) {
    if (client_->IsRunning() && !client_->IsShuttingDown()) {
      client_->BeginShutdown();
    }
    return;
  }
  // Prefer a graceful terminate so a launched debuggee can shut down cleanly;
  // the adapter then emits `terminated`, which drives the disconnect. Adapters
  // without terminate support are disconnected directly.
  if (client_->Capabilities().supports_terminate_request) {
    client_->SendRequestAsync("terminate", util::JsonValue(nullptr), {});
  } else {
    client_->BeginShutdown();
    SetState(State::Terminated);
  }
}

void DebugSession::RequestStackTrace(const dap_protocol::DapStoppedEvent& stop) {
  if (!callbacks_.on_stopped) {
    return;
  }
  client_->SendRequestAsync(
      "stackTrace", dap_protocol::MakeStackTraceArguments(stop.thread_id, 0, 0),
      [this, stop](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        if (callbacks_.on_stopped) {
          callbacks_.on_stopped(stop, dap_protocol::ParseStackFrames(response.body));
        }
      });
}

void DebugSession::RequestScopes(int frame_id,
                                 std::function<void(std::vector<dap_protocol::DapScope>)> callback) {
  if (!callback || !client_->IsInitialized()) {
    return;
  }
  client_->SendRequestAsync(
      "scopes", dap_protocol::MakeScopesArguments(frame_id),
      [callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        callback(dap_protocol::ParseScopes(response.body));
      });
}

void DebugSession::RequestVariables(
    int variables_reference, std::function<void(std::vector<dap_protocol::DapVariable>)> callback) {
  if (!callback || variables_reference <= 0 || !client_->IsInitialized()) {
    return;
  }
  client_->SendRequestAsync(
      "variables", dap_protocol::MakeVariablesArguments(variables_reference, 0, 0),
      [callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        callback(dap_protocol::ParseVariables(response.body));
      });
}

void DebugSession::SetVariable(int variables_reference, const std::string& name,
                               const std::string& value,
                               std::function<void(bool, dap_protocol::DapSetVariableResult)> callback) {
  // Gate on the adapter capability so we never send a request it would reject.
  if (variables_reference <= 0 || !client_->IsInitialized() ||
      !client_->Capabilities().supports_set_variable) {
    if (callback) {
      callback(false, dap_protocol::DapSetVariableResult{});
    }
    return;
  }
  client_->SendRequestAsync(
      "setVariable",
      dap_protocol::MakeSetVariableArguments(
          dap_protocol::SetVariableInput{
              .variables_reference = variables_reference, .name = name, .value = value}),
      [callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!callback) {
          return;
        }
        callback(response.success,
                 response.success ? dap_protocol::ParseSetVariableResult(response.body)
                                  : dap_protocol::DapSetVariableResult{});
      });
}

void DebugSession::RequestEvaluate(
    const std::string& expression, int frame_id, const std::string& context,
    std::function<void(bool, dap_protocol::DapEvaluateResult)> callback) {
  // Gate hover evaluation on the adapter capability so we never send a request it
  // would reject; non-hover contexts (watch/repl, later phases) are always allowed.
  const bool hover = context == "hover";
  if (expression.empty() || !client_->IsInitialized() ||
      (hover && !client_->Capabilities().supports_evaluate_for_hovers)) {
    if (callback) {
      callback(false, dap_protocol::DapEvaluateResult{});
    }
    return;
  }
  client_->SendRequestAsync(
      "evaluate", dap_protocol::MakeEvaluateArguments(expression, frame_id, context),
      [callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!callback) {
          return;
        }
        callback(response.success, response.success
                                       ? dap_protocol::ParseEvaluateResult(response.body)
                                       : dap_protocol::DapEvaluateResult{});
      });
}

void DebugSession::SendResumeRequest(const char* command) {
  if (state_ != State::Stopped) {
    return;
  }
  client_->SendRequestAsync(command, ThreadIdArgs(stopped_thread_id_), {});
  // Optimistically resume so the UI clears immediately; the next `stopped`
  // (breakpoint/step end) repopulates the execution state.
  if (callbacks_.on_resumed) {
    callbacks_.on_resumed();
  }
  SetState(State::Running);
}

void DebugSession::Continue() { SendResumeRequest("continue"); }
void DebugSession::StepOver() { SendResumeRequest("next"); }
void DebugSession::StepIn() { SendResumeRequest("stepIn"); }
void DebugSession::StepOut() { SendResumeRequest("stepOut"); }

void DebugSession::Pause() {
  if (state_ != State::Running || !client_->IsInitialized()) {
    return;
  }
  // `pause` requires a threadId. When running we have no stopped thread, so ask
  // the adapter for its threads and pause the first one (this is where Phase 3's
  // `threads` request naturally lands; a thread selector arrives in Phase 7).
  client_->SendRequestAsync(
      "threads", util::JsonValue(nullptr), [this](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        const std::vector<dap_protocol::DapThread> threads =
            dap_protocol::ParseThreads(response.body);
        if (threads.empty()) {
          return;
        }
        client_->SendRequestAsync("pause", ThreadIdArgs(threads.front().id), {});
      });
}

void DebugSession::DrainCallbacks() { client_->DrainCallbacks(); }

void DebugSession::SetState(State state) {
  if (state_ == state) {
    return;
  }
  // Terminal states are absorbing: once failed/terminated, stay there.
  if (IsTerminalState(state_)) {
    return;
  }
  state_ = state;
  if (callbacks_.on_state_changed) {
    callbacks_.on_state_changed(state_);
  }
}

}  // namespace microide::workspace
