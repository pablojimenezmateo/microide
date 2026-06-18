#include "workspace/DebugSession.h"

#include <algorithm>
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
  // NB: the launch/attach request is NOT sent here. It is deferred until the
  // configuration phase completes (breakpoints + configurationDone) in the
  // `initialized` handler. Some adapters (notably gdb's `--interpreter=dap`) run
  // the debuggee *during* the launch request rather than waiting for
  // configurationDone, so sending launch early would let the program race past
  // breakpoints before they are armed (and make configurationDone fail with
  // `notStopped`). Sending launch last arms breakpoints first and is accepted by
  // adapters that emit `initialized` independently of launch (gdb, lldb-dap,
  // debugpy, and the in-tree mock adapter all do).
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
                              // Launch is sent after configurationDone; for adapters
                              // that run on launch, a `stopped` event may already have
                              // moved us to Stopped before this response. Only converge
                              // to Running if still mid-handshake.
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

void DebugSession::SendExceptionFilters() {
  const std::vector<dap_protocol::DapExceptionFilter>& advertised =
      client_->Capabilities().exception_filters;
  if (advertised.empty() || !callbacks_.exception_filter_provider) {
    return;
  }
  const std::vector<std::string> enabled = callbacks_.exception_filter_provider();
  // Send only ids the adapter advertised, in advertised order, so we never push a
  // filter id the adapter would reject.
  std::vector<std::string> ids;
  for (const dap_protocol::DapExceptionFilter& filter : advertised) {
    if (std::find(enabled.begin(), enabled.end(), filter.filter) != enabled.end()) {
      ids.push_back(filter.filter);
    }
  }
  client_->SendRequestAsync("setExceptionBreakpoints",
                            dap_protocol::MakeSetExceptionBreakpointsArguments(ids), {});
}

void DebugSession::ResendExceptionFilters() {
  if (!client_->IsInitialized()) {
    return;
  }
  SendExceptionFilters();
}

void DebugSession::HandleEvent(const std::string& event, const util::JsonValue& body) {
  if (event == "initialized") {
    // The adapter is ready for configuration. Surface its advertised exception
    // filters (so the host can seed/show them), install breakpoints + exception
    // filters, then finalize with configurationDone. All ride the client's single
    // ordered stream, so configurationDone reaches the adapter last (the DAP
    // handshake requires send-order, not response-order); verification reflects
    // back asynchronously.
    if (callbacks_.on_exception_filters_available) {
      callbacks_.on_exception_filters_available(client_->Capabilities().exception_filters);
    }
    SendAllBreakpoints();
    SendExceptionFilters();
    SendConfigurationDone();
    // Launch/attach goes out last — after breakpoints + configurationDone — so an
    // adapter that starts the debuggee on the launch request (e.g. gdb DAP) runs
    // with breakpoints already armed. See the note in Start().
    SendLaunchRequest();
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
    last_stop_ = stop;
    SetState(State::Stopped);
    // Report the halt immediately with the real reason/thread, before the async
    // stackTrace round-trip — push observers must not wait for a slow adapter.
    if (callbacks_.on_stop_began) {
      callbacks_.on_stop_began(stop);
    }
    // Resolve the call stack for the stopped thread, then hand the focused
    // frames up so the host can highlight the execution line + fill the panel.
    RequestStackTrace(stop);
    // Fetch the thread list as a second request so the Call Stack panel can show
    // a thread selector; it lands a beat later and never delays the stack.
    RequestThreadsForStop();
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
  if (expression.empty() || !client_->IsInitialized()) {
    if (callback) {
      callback(false, dap_protocol::DapEvaluateResult{});
    }
    return;
  }
  // Hover-to-inspect: the DAP spec says clients should only send context "hover"
  // when the adapter advertises supportsEvaluateForHovers, but several common
  // adapters (notably GDB) evaluate fine without advertising it. Rather than drop
  // the request, fall back to the universally-supported "repl" context so hovering
  // a symbol still resolves a value. (GDB resolves statics/members this way too.)
  std::string effective_context = context;
  if (context == "hover" && !client_->Capabilities().supports_evaluate_for_hovers) {
    effective_context = "repl";
  }
  client_->SendRequestAsync(
      "evaluate", dap_protocol::MakeEvaluateArguments(expression, frame_id, effective_context),
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
  // the adapter for its threads and pause the first one.
  RequestThreads([this](std::vector<dap_protocol::DapThread> threads) {
    if (threads.empty()) {
      return;
    }
    client_->SendRequestAsync("pause", ThreadIdArgs(threads.front().id), {});
  });
}

void DebugSession::Restart() {
  if (!client_->IsInitialized() || !client_->Capabilities().supports_restart_request) {
    return;
  }
  // A re-emitted `initialized` (some adapters send one on restart) must re-install
  // breakpoints and re-finalize, so re-arm the configurationDone guard.
  configuration_done_sent_ = false;
  // The DAP `restart` request optionally carries the original launch/attach
  // arguments under `arguments`; pass them through when we have them.
  util::JsonObject args;
  if (!config_.arguments.IsNull()) {
    args["arguments"] = config_.arguments;
  }
  client_->SendRequestAsync("restart", util::JsonValue(std::move(args)), {});
  // Optimistically clear the current stop; the adapter re-runs and stops again.
  if (state_ == State::Stopped) {
    if (callbacks_.on_resumed) {
      callbacks_.on_resumed();
    }
    SetState(State::Running);
  }
}

void DebugSession::RequestThreads(
    std::function<void(std::vector<dap_protocol::DapThread>)> callback) {
  if (!callback || !client_->IsInitialized()) {
    return;
  }
  client_->SendRequestAsync(
      "threads", util::JsonValue(nullptr),
      [callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        callback(dap_protocol::ParseThreads(response.body));
      });
}

void DebugSession::RequestThreadsForStop() {
  if (!callbacks_.on_threads) {
    return;
  }
  RequestThreads([this](std::vector<dap_protocol::DapThread> threads) {
    if (callbacks_.on_threads) {
      callbacks_.on_threads(threads);
    }
  });
}

void DebugSession::SwitchThread(int thread_id) {
  if (state_ != State::Stopped || !client_->IsInitialized()) {
    return;
  }
  stopped_thread_id_ = thread_id;
  dap_protocol::DapStoppedEvent stop = last_stop_;
  stop.thread_id = thread_id;
  last_stop_ = stop;
  // Reuse the stop path: re-resolve frames for the picked thread and re-emit
  // on_stopped so the host re-focuses it (execution line + Call Stack + scopes).
  RequestStackTrace(stop);
}

void DebugSession::Reactivate() {
  if (state_ != State::Stopped || !client_->IsInitialized()) {
    return;
  }
  // Re-resolve the call stack + thread list for the retained stop so the shared
  // execution view (which another session may have overwritten while this one was
  // in the background) rebuilds for this session's current location.
  RequestStackTrace(last_stop_);
  RequestThreadsForStop();
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
