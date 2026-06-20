#include "workspace/DebugSession.h"

#include <chrono>
#include <cstdio>
#include <utility>

#include "util/JsonValue.h"
#include "util/PerformanceTrace.h"

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
  is_gdb_adapter_ = false;
  for (const std::string& token : command) {
    if (token.find("gdb") != std::string::npos) {
      is_gdb_adapter_ = true;
      break;
    }
  }

  client_->SetEventCallback(
      [this](const std::string& event, const util::JsonValue& body) { HandleEvent(event, body); });

  if (!client_->Start(command, config_.type, cwd, sandbox)) {
    std::string reason = client_->LastError();
    if (reason.empty()) {
      reason = "debug adapter failed to start";
    }
    // The client never started, so there is no I/O thread to shut down.
    TransitionToTerminal(State::Failed, std::move(reason));
    return false;
  }

  SetState(State::Initializing);
  // NB: the launch/attach request is NOT sent here. The full launch/configuration
  // handshake is driven from the `initialized` event handler, where the adapter's
  // capabilities are known. A spec-compliant adapter emits `initialized` from the
  // `initialize` request (independent of launch), so deferring launch to that
  // handler is safe; see the ordering rationale there.
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
                              // Launch is sent before configurationDone; the adapter
                              // runs the debuggee on configurationDone, so a `stopped`
                              // event may already have moved us to Stopped before this
                              // response. Only converge to Running if still mid-handshake.
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

void DebugSession::SendDebuggerValueLimits() {
  if (!is_gdb_adapter_) {
    return;
  }
  // Bound how much gdb will format/read for a single value. An uninitialized or
  // corrupt STL container reads a garbage size (the probe saw a std::vector report
  // a length of billions); formatting that unbounded is what froze the host. The
  // load-bearing cap is `max-value-size`: with it at 1 MiB a top-of-`main` expand
  // (locals still uninitialized) took 15 s+; at 64 KiB the same expand returned in
  // ~0.3 s. The `print` caps bound aggregate/string/repeat expansion on top.
  // These are global gdb settings, issued as REPL commands before the program runs.
  // Pass frame_id = -1 so `MakeEvaluateArguments` omits `frameId`: a present
  // `frameId:0` before launch (no frames exist yet) makes gdb's DAP error with
  // "list index out of range", so the omission is required for the settings to
  // actually apply. (Frame 0 is a valid frame *after* stopping; -1 means "no frame".)
  // No callback — best-effort; a non-gdb adapter never reaches here.
  static constexpr const char* kLimitCommands[] = {
      "set max-value-size 65536",
      "set print elements 200",
      "set print characters 200",
      "set print repeats 10",
  };
  for (const char* command : kLimitCommands) {
    client_->SendRequestAsync("evaluate",
                              dap_protocol::MakeEvaluateArguments(command, /*frame_id=*/-1, "repl"),
                              {});
  }
}

// Breakpoint / exception-filter send paths (SendAllBreakpoints,
// Send/ResendBreakpointsForFile, Send/ResendFunctionBreakpoints,
// Send/ResendExceptionFilters) live in DebugSessionBreakpoints.cpp.

void DebugSession::HandleEvent(const std::string& event, const util::JsonValue& body) {
  if (event == "initialized") {
    // The adapter is ready for configuration. Surface its advertised exception
    // filters (so the host can seed/show them), then run the DAP configuration
    // handshake. All requests ride the client's single ordered stream, so they
    // reach the adapter in call order (the handshake requires send-order, not
    // response-order); verification reflects back asynchronously.
    if (callbacks_.on_exception_filters_available) {
      callbacks_.on_exception_filters_available(client_->Capabilities().exception_filters);
    }
    // Clamp gdb value formatting before the program runs, so the first expand of a
    // (possibly uninitialized) local cannot trigger unbounded formatting.
    SendDebuggerValueLimits();
    // DAP handshake ordering: the launch/attach request must be in flight *before*
    // configurationDone. A spec-compliant adapter (gdb, lldb-dap, debugpy) defers
    // running the debuggee until configurationDone and rejects a configurationDone
    // that arrives with no launch pending — gdb's DAP raises "launch or attach not
    // specified" and then never answers the late launch, hanging the session. Since
    // the run is gated on configurationDone, breakpoints sent in between are still
    // armed first. An adapter with no configuration phase starts the debuggee on
    // the launch request itself, so there breakpoints must precede launch.
    const bool launch_first = client_->Capabilities().supports_configuration_done_request;
    if (launch_first) {
      SendLaunchRequest();
    }
    SendAllBreakpoints();
    SendFunctionBreakpoints();
    SendExceptionFilters();
    if (!launch_first) {
      SendLaunchRequest();
    }
    SendConfigurationDone();
  } else if (event == "output") {
    if (callbacks_.on_output) {
      callbacks_.on_output(dap_protocol::ParseOutputEvent(body));
    }
  } else if (event == "terminated" || event == "exited") {
    // Clean end: no reason. TransitionToTerminal absorbs a duplicate event, and
    // BeginShutdown is idempotent.
    TransitionToTerminal(State::Terminated, {});
    client_->BeginShutdown();
  } else if (event == "stopped") {
    const dap_protocol::DapStoppedEvent stop = dap_protocol::ParseStoppedEvent(body);
    stopped_thread_id_ = stop.thread_id;
    last_stop_ = stop;
    ++stop_epoch_;
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
    RefreshThreadList();
  } else if (event == "continued") {
    if (state_ == State::Stopped) {
      if (callbacks_.on_resumed) {
        callbacks_.on_resumed();
      }
      SetState(State::Running);
    }
  } else if (event == "breakpoint") {
    // Async breakpoint update: the adapter bound, relocated, or invalidated a
    // breakpoint after the initial setBreakpoints response (e.g. a shared library
    // loaded later, or a condition was rejected). Reflect it so the gutter/panel
    // stop showing a stale "unverified" state with no explanation.
    if (callbacks_.on_breakpoint_changed) {
      const util::JsonValue& bp = body["breakpoint"];
      const util::JsonValue& source_path = bp["source"]["path"];
      std::filesystem::path path;
      if (source_path.IsString() && !source_path.AsString().empty()) {
        path = std::filesystem::path(source_path.AsString());
      }
      callbacks_.on_breakpoint_changed(path, dap_protocol::ParseBreakpoint(bp));
    }
  } else if (event == "thread") {
    // A thread started or exited. Refresh the thread list so the Call Stack thread
    // selector stays current between stops (a stop already refreshes it). Reuses
    // the stop-epoch guard, so a refresh superseded by a new stop is dropped.
    RefreshThreadList();
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
  // Prefer a graceful terminate so a launched debuggee can shut down cleanly;
  // the adapter then emits `terminated`, which drives the disconnect. Adapters
  // without terminate support are disconnected directly.
  if (client_->Capabilities().supports_terminate_request) {
    client_->SendRequestAsync("terminate", util::JsonValue(nullptr), {});
  } else {
    client_->BeginShutdown();
    TransitionToTerminal(State::Terminated, {});
  }
}

void DebugSession::NotifyProcessExited() {
  if (state_ == State::Inactive || IsTerminalState(state_)) {
    return;  // already terminal, or never started — nothing to reconcile
  }
  // The adapter process is gone but no DAP `terminated`/`exited` arrived. A clean
  // exit after we asked to stop is Terminated; an exit while we still expected the
  // adapter alive (crash / external kill / RLIMIT_AS) is a Failed with a reason.
  if (stop_requested_) {
    TransitionToTerminal(State::Terminated, {});
  } else {
    // Keep an earlier, more specific error if one was already recorded; otherwise
    // fall back to the generic reason. (Empty reason leaves last_error_ untouched.)
    TransitionToTerminal(State::Failed,
                         last_error_.empty() ? "debug adapter exited unexpectedly" : std::string());
  }
  // Ensure the I/O thread is joined so PruneTerminated can reap the session.
  if (!client_->IsShuttingDown()) {
    client_->BeginShutdown();
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
          // Surface the failure so the caller clears the loading placeholder
          // instead of leaving a spinner that never resolves.
          callback(false, {});
          return;
        }
        std::vector<dap_protocol::DapVariable> variables =
            dap_protocol::ParseVariables(response.body);
        // Round-trip timing (adapter-side cost) is the suspect for slow expands.
        // Env-gated so it is silent unless MICROIDE_PERF_TRACE is set.
        if (util::PerformanceTrace::Enabled()) {
          const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - request_started)
                                        .count();
          std::fprintf(stderr, "[perf] %8.2f ms | dap variables ref=%d children=%zu\n", elapsed_ms,
                       variables_reference, variables.size());
          std::fflush(stderr);
        }
        callback(true, std::move(variables));
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

void DebugSession::ReverseContinue() {
  if (!client_->Capabilities().supports_step_back) {
    return;
  }
  SendResumeRequest("reverseContinue");
}

void DebugSession::StepBack() {
  if (!client_->Capabilities().supports_step_back) {
    return;
  }
  SendResumeRequest("stepBack");
}

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

void DebugSession::RefreshThreadList() {
  if (!callbacks_.on_threads) {
    return;
  }
  // Capture the stop this list belongs to. If a newer stop (or thread switch /
  // reactivate) lands before this async response, drop it so the selector is never
  // repopulated for a superseded stop.
  const std::uint64_t epoch = stop_epoch_;
  RequestThreads([this, epoch](std::vector<dap_protocol::DapThread> threads) {
    if (epoch != stop_epoch_) {
      return;
    }
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
  // in the background) rebuilds for this session's current location. Bump the stop
  // epoch so this re-projection's thread list supersedes any still-in-flight one.
  ++stop_epoch_;
  RequestStackTrace(last_stop_);
  RefreshThreadList();
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

void DebugSession::TransitionToTerminal(State terminal_state, std::string reason) {
  // Absorbing: the first terminal transition wins, so on_terminated fires once.
  if (IsTerminalState(state_)) {
    return;
  }
  if (!reason.empty()) {
    last_error_ = std::move(reason);
  }
  SetState(terminal_state);
  if (callbacks_.on_terminated) {
    callbacks_.on_terminated(terminal_state, last_error_);
  }
}

}  // namespace microide::workspace
