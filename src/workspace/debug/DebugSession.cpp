#include "workspace/debug/DebugSession.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <utility>

#include "util/JsonValue.h"
#include "util/PerformanceTrace.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

bool ContainsGdbCaseInsensitive(std::string_view text) {
  for (std::size_t i = 0; i + 3 <= text.size(); ++i) {
    if ((text[i] == 'g' || text[i] == 'G') && (text[i + 1] == 'd' || text[i + 1] == 'D') &&
        (text[i + 2] == 'b' || text[i + 2] == 'B')) {
      return true;
    }
  }
  return false;
}

bool CommandLooksLikeGdb(const std::vector<std::string>& command,
                         const std::string& adapter_type) {
  if (ContainsGdbCaseInsensitive(adapter_type)) {
    return true;
  }
  for (const std::string& token : command) {
    const std::size_t slash = token.find_last_of("/\\");
    const std::string_view basename = slash == std::string::npos
                                          ? std::string_view(token)
                                          : std::string_view(token).substr(slash + 1);
    if (ContainsGdbCaseInsensitive(basename)) {
      return true;
    }
  }
  return false;
}

bool IsTerminalState(DebugSession::State state) {
  return state == DebugSession::State::Terminated || state == DebugSession::State::Failed;
}

}

DebugSession::DebugSession() : client_(std::make_unique<DapClient>()) {}

DebugSession::DebugSession(std::unique_ptr<DapClient> client) : client_(std::move(client)) {
  if (client_ == nullptr) {
    client_ = std::make_unique<DapClient>();
  }
}

DebugSession::~DebugSession() = default;

void DebugSession::SetWakeChannel(Uint32 event_type) { client_->SetWakeChannel(event_type); }

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

bool DebugSession::Start(const platform::ProcessLauncher& launcher,
                         const std::vector<std::string>& command, const LaunchConfig& config,
                         const std::string& cwd, const platform::SubprocessSandbox& sandbox) {
  config_ = config;
  launch_sent_ = false;
  configuration_done_sent_ = false;
  configuration_done_answered_ = false;
  local_adapter_ = launcher.is_local();
  debuggee_pid_ = 0;
  last_error_.clear();
  is_gdb_adapter_ = CommandLooksLikeGdb(command, config.type);
  client_->SetTerminateDebuggeeOnDisconnect(!config_.IsAttach());

  client_->SetEventCallback(
      [this](const std::string& event, const util::JsonValue& body) { HandleEvent(event, body); });

  if (!client_->Start(launcher, command, config_.type, cwd, sandbox)) {
    std::string reason = client_->LastError();
    if (reason.empty()) {
      reason = "debug adapter failed to start";
    }
    TransitionToTerminal(State::Failed, std::move(reason));
    return false;
  }

  SetState(State::Initializing);
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
                                TransitionToTerminal(
                                    State::Failed,
                                    response.message.empty() ? (verb + " request was rejected")
                                                             : response.message);
                                client_->BeginShutdown();
                                return;
                              }
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
                              configuration_done_answered_ = true;
                              const bool in_handshake = state_ == State::Configuring ||
                                                        state_ == State::Initializing;
                              if (!response.success) {
                                if (in_handshake) {
                                  TransitionToTerminal(State::Failed,
                                                       response.message.empty()
                                                           ? "configurationDone was rejected"
                                                           : response.message);
                                  client_->BeginShutdown();
                                }
                                return;
                              }
                              if (in_handshake) {
                                SetState(State::Running);
                              }
                            });
}

void DebugSession::SendDebuggerValueLimits() {
  if (!is_gdb_adapter_) {
    return;
  }
  static constexpr const char* kLimitCommands[] = {
      "set max-value-size 65536",
      "set print elements 200",
      "set print characters 200",
      "set print repeats 10",
  };
  for (const char* command : kLimitCommands) {
    client_->SendRequestAsync("evaluate",
                              dap_protocol::MakeEvaluateArguments(command, -1, "repl"),
                              {});
  }
}

void DebugSession::HandleEvent(const std::string& event, const util::JsonValue& body) {
  if (event == "initialized") {
    if (callbacks_.on_exception_filters_available) {
      callbacks_.on_exception_filters_available(client_->Capabilities().exception_filters);
    }
    SendDebuggerValueLimits();
    SendAllBreakpoints();
    SendFunctionBreakpoints();
    SendExceptionFilters();
    SendLaunchRequest();
    SendConfigurationDone();
  } else if (event == "output") {
    if (callbacks_.on_output) {
      callbacks_.on_output(dap_protocol::ParseOutputEvent(body));
    }
  } else if (event == "terminated" || event == "exited") {
    TransitionToTerminal(State::Terminated, {});
    client_->BeginShutdown();
  } else if (event == "stopped") {
    const dap_protocol::DapStoppedEvent stop = dap_protocol::ParseStoppedEvent(body);
    stopped_thread_id_ = stop.thread_id;
    last_stop_ = stop;
    ++stop_epoch_;
    SetState(State::Stopped);
    util::InvokeIfSet(callbacks_.on_stop_began, stop);
    if (stop.thread_id.has_value()) {
      RequestStackTrace(stop);
      RefreshThreadList();
    } else {
      ResolveFocusThreadForStop(stop_epoch_);
    }
  } else if (event == "continued") {
    const dap_protocol::DapContinuedEvent resumed = dap_protocol::ParseContinuedEvent(body);
    if (state_ == State::Stopped && resumed.all_threads_continued) {
      util::InvokeIfSet(callbacks_.on_resumed);
      SetState(State::Running);
    }
  } else if (event == "breakpoint") {
    if (callbacks_.on_breakpoint_changed) {
      const util::JsonValue& bp = body["breakpoint"];
      const util::JsonValue& source_path = bp["source"]["path"];
      std::filesystem::path path;
      if (source_path.IsString() && !source_path.AsString().empty()) {
        path = std::filesystem::path(source_path.AsString());
      }
      callbacks_.on_breakpoint_changed(path, dap_protocol::ParseBreakpoint(bp));
    }
  } else if (event == "process") {
    const util::JsonValue& pid = body["systemProcessId"];
    const util::JsonValue& is_local = body["isLocalProcess"];
    if (local_adapter_ && pid.IsInt() && (!is_local.IsBool() || is_local.AsBool())) {
      debuggee_pid_ = static_cast<int>(pid.AsInt());
    }
  } else if (event == "thread") {
    if (const util::JsonValue& id = body["threadId"]; id.IsInt()) {
      const int thread_id = static_cast<int>(id.AsInt());
      const auto known = std::find(known_thread_ids_.begin(), known_thread_ids_.end(), thread_id);
      if (body["reason"].IsString() && body["reason"].AsString() == "exited") {
        if (known != known_thread_ids_.end()) {
          known_thread_ids_.erase(known);
        }
      } else if (known == known_thread_ids_.end()) {
        known_thread_ids_.push_back(thread_id);
      }
    }
    if (pause_pending_ && state_ == State::Running && !known_thread_ids_.empty()) {
      pause_pending_ = false;
      client_->SendRequestAsync("pause", dap_protocol::MakeThreadIdArguments(known_thread_ids_.front()), {});
    }
    if (state_ == State::Stopped) {
      RefreshThreadList();
    }
  } else if (event == "capabilities") {
    client_->ApplyCapabilitiesUpdate(body["capabilities"]);
    if (callbacks_.on_exception_filters_available) {
      callbacks_.on_exception_filters_available(client_->Capabilities().exception_filters);
    }
    util::InvokeIfSet(callbacks_.on_capabilities_changed);
  }
}

void DebugSession::RequestStop() {
  stop_requested_ = true;
  if (state_ == State::Inactive || IsTerminalState(state_)) {
    if (client_->IsRunning() && !client_->IsShuttingDown()) {
      client_->BeginShutdown();
    }
    return;
  }
  if (client_->Capabilities().supports_terminate_request) {
    (void)InterruptGdbStillStartingProgram();
    client_->SendRequestAsync("terminate", util::JsonValue(nullptr),
                              [this](const dap_protocol::DapResponse& response) {
                                if (response.success || IsTerminalState(state_)) {
                                  return;
                                }
                                client_->BeginShutdown();
                                TransitionToTerminal(State::Terminated, {});
                              });
  } else {
    client_->BeginShutdown();
    TransitionToTerminal(State::Terminated, {});
  }
}

void DebugSession::NotifyProcessExited() {
  if (state_ == State::Inactive || IsTerminalState(state_)) {
    return;
  }
  if (stop_requested_) {
    TransitionToTerminal(State::Terminated, {});
  } else {
    TransitionToTerminal(State::Failed,
                         last_error_.empty() ? "debug adapter exited unexpectedly" : std::string());
  }
  if (!client_->IsShuttingDown()) {
    client_->BeginShutdown();
  }
}

void DebugSession::RequestStackTrace(const dap_protocol::DapStoppedEvent& stop) {
  if (!callbacks_.on_stopped) {
    return;
  }
  if (!stop.thread_id.has_value()) {
    return;
  }
  const int thread_id = *stop.thread_id;
  const std::uint64_t epoch = stop_epoch_;
  client_->SendRequestAsync(
      "stackTrace", dap_protocol::MakeStackTraceArguments(thread_id, 0, 10000),
      [this, stop, epoch](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        if (state_ != State::Stopped || epoch != stop_epoch_) {
          return;
        }
        callbacks_.on_stopped(stop, dap_protocol::ParseStackFrames(response.body));
      });
}

void DebugSession::ResolveFocusThreadForStop(std::uint64_t epoch) {
  RequestThreads([this, epoch](std::vector<dap_protocol::DapThread> threads) {
    if (epoch != stop_epoch_ || state_ != State::Stopped) {
      return;
    }
    if (threads.empty()) {
      return;
    }
    const int focus = threads.front().id;
    stopped_thread_id_ = focus;
    last_stop_.thread_id = focus;
    RequestStackTrace(last_stop_);
    RefreshThreadList();
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
    int variables_reference, int start, int count,
    std::function<void(bool, std::vector<dap_protocol::DapVariable>)> callback) {
  if (!callback) {
    return;
  }
  if (variables_reference <= 0 || !client_->IsInitialized()) {
    callback(false, {});
    return;
  }
  const auto request_started = std::chrono::steady_clock::now();
  client_->SendRequestAsync(
      "variables", dap_protocol::MakeVariablesArguments(variables_reference, start, count),
      [callback = std::move(callback), variables_reference,
       request_started](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          callback(false, {});
          return;
        }
        std::vector<dap_protocol::DapVariable> variables =
            dap_protocol::ParseVariables(response.body);
        if (util::PerformanceTrace::Enabled()) {
          const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - request_started)
                                        .count();
          util::PerformanceTrace::RecordSample("dap::VariablesRoundTrip", elapsed_ms);
          if (util::PerformanceTrace::StreamEnabled()) {
            std::fprintf(stderr, "[perf] %8.2f ms | dap variables ref=%d children=%zu\n",
                         elapsed_ms, variables_reference, variables.size());
            std::fflush(stderr);
          }
        }
        callback(true, std::move(variables));
      });
}

void DebugSession::SetVariable(int variables_reference, const std::string& name,
                               const std::string& value,
                               std::function<void(bool, dap_protocol::DapSetVariableResult)> callback) {
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

void DebugSession::RequestThreads(
    std::function<void(std::vector<dap_protocol::DapThread>)> callback) {
  if (!callback || !client_->IsInitialized()) {
    return;
  }
  client_->SendRequestAsync(
      "threads", util::JsonValue(nullptr),
      [this, callback = std::move(callback)](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        std::vector<dap_protocol::DapThread> threads = dap_protocol::ParseThreads(response.body);
        known_thread_ids_.clear();
        for (const dap_protocol::DapThread& thread : threads) {
          known_thread_ids_.push_back(thread.id);
        }
        callback(std::move(threads));
      });
}

void DebugSession::RefreshThreadList() {
  if (!callbacks_.on_threads) {
    return;
  }
  const std::uint64_t epoch = stop_epoch_;
  RequestThreads([this, epoch](std::vector<dap_protocol::DapThread> threads) {
    if (epoch != stop_epoch_ || state_ != State::Stopped) {
      return;
    }
    util::InvokeIfSet(callbacks_.on_threads, threads);
  });
}

void DebugSession::SwitchThread(int thread_id) {
  if (state_ != State::Stopped || !client_->IsInitialized()) {
    return;
  }
  stopped_thread_id_ = thread_id;
  last_stop_.thread_id = thread_id;
  const dap_protocol::DapStoppedEvent stop = last_stop_;
  ++stop_epoch_;
  RequestStackTrace(stop);
}

void DebugSession::Reactivate() {
  if (state_ != State::Stopped || !client_->IsInitialized()) {
    return;
  }
  ++stop_epoch_;
  RequestStackTrace(last_stop_);
  RefreshThreadList();
}

void DebugSession::DrainCallbacks() { client_->DrainCallbacks(); }

void DebugSession::SetState(State state) {
  if (state_ == state) {
    return;
  }
  if (IsTerminalState(state_)) {
    return;
  }
  state_ = state;
  util::InvokeIfSet(callbacks_.on_state_changed, state_);
}

void DebugSession::TransitionToTerminal(State terminal_state, std::string reason) {
  if (IsTerminalState(state_)) {
    return;
  }
  if (!reason.empty()) {
    last_error_ = std::move(reason);
  }
  SetState(terminal_state);
  util::InvokeIfSet(callbacks_.on_terminated, terminal_state, last_error_);
}

}
