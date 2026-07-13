#include "workspace/DebugSession.h"

#include <chrono>
#include <cstdio>
#include <string_view>
#include <utility>

#include "util/JsonValue.h"
#include "util/PerformanceTrace.h"

namespace microide::workspace {

namespace {

// Detect whether a debug adapter is gdb so the pre-run value-size caps that keep
// gdb from freezing the host for 15s+ while expanding an uninitialized STL
// container get sent (see DebugSession::SendDebuggerValueLimits). Match the
// executable *basename* (not any substring, which false-positives on paths like
// /home/gdbuser/bin/lldb-dap) plus the host-controlled adapter type id; err
// toward detection because a false negative reinstates the freeze risk.
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
  is_gdb_adapter_ = CommandLooksLikeGdb(command, config.type);

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
                              const bool in_handshake = state_ == State::Configuring ||
                                                        state_ == State::Initializing;
                              if (!response.success) {
                                // A rejected configurationDone *during the handshake* means
                                // launch/configuration was invalid and the debuggee will not
                                // run: treat it as terminal instead of showing a Running
                                // session that never starts (mirrors the launch-failure path).
                                // If we already reached Running (launch response landed first),
                                // leave a synthetic failure — e.g. the adapter dying — to the
                                // reconciliation path so it is not double-handled here.
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
    if (stop.thread_id.has_value()) {
      // Resolve the call stack for the stopped thread, then hand the focused
      // frames up so the host can highlight the execution line + fill the panel.
      RequestStackTrace(stop);
      // Fetch the thread list as a second request so the Call Stack panel can show
      // a thread selector; it lands a beat later and never delays the stack.
      RefreshThreadList();
    } else {
      // The adapter omitted threadId (legal on `stopped`). Resolve a concrete
      // thread before requesting the stack/continue instead of sending threadId:0.
      ResolveFocusThreadForStop(stop_epoch_);
    }
  } else if (event == "continued") {
    // Only a full resume (allThreadsContinued) tears down the shared stopped view.
    // A single-thread continue must NOT clear the focused stack/execution state:
    // microide focuses one stopped thread at a time, so a partial resume of some
    // other thread leaves that focus valid until it too stops or fully resumes.
    // (An absent allThreadsContinued is treated as not-all — the conservative
    // choice; the next `stopped` or full `continued` re-syncs the view.)
    const dap_protocol::DapContinuedEvent resumed = dap_protocol::ParseContinuedEvent(body);
    if (state_ == State::Stopped && resumed.all_threads_continued) {
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
  } else if (event == "capabilities") {
    // The adapter changed its advertised capabilities after initialize. gdb does
    // exactly this when reverse execution becomes available (an rr replay target,
    // or gdb's own `record`), sending a *partial* body
    // {"capabilities":{"supportsStepBack":true}}. Merge it and notify the host so
    // the reverse-debug UI — gated live on supportsStepBack — repaints immediately
    // instead of waiting for the next input event.
    client_->ApplyCapabilitiesUpdate(body["capabilities"]);
    // Re-surface exception filters in case the update restated them.
    if (callbacks_.on_exception_filters_available) {
      callbacks_.on_exception_filters_available(client_->Capabilities().exception_filters);
    }
    if (callbacks_.on_capabilities_changed) {
      callbacks_.on_capabilities_changed();
    }
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
  // A concrete threadId is required; the no-threadId stop path resolves one via
  // ResolveFocusThreadForStop before calling here, so an unset id means "nothing
  // to focus yet" and we skip rather than send threadId:0.
  if (!stop.thread_id.has_value()) {
    return;
  }
  const int thread_id = *stop.thread_id;
  // Bind this stack request to the stop it belongs to. Every other async apply
  // path is generation-guarded — scopes/variables/watches via frame_generation_,
  // the thread list via stop_epoch_ (RefreshThreadList) — but the stack trace,
  // which drives the editor execution highlight, the frame focus, and the
  // notify_stop_resolved push, was applied unconditionally. If the user
  // resumes/steps before a slow adapter answers stackTrace, the late response
  // would re-project a full "stopped" view onto a now-running session (spurious
  // execution highlight + focus jump + stop-resolved broadcast) that self-heals
  // only on the next real stop. Drop the response unless we are still stopped at
  // the same epoch. Note a plain resume (SendResumeRequest) flips state_ to
  // Running WITHOUT bumping stop_epoch_, so the state_ check is load-bearing on
  // its own; the epoch check additionally drops a stack superseded by a newer stop
  // or thread switch. Both members are only touched on the main thread (mirrors
  // RefreshThreadList's unsynchronized stop_epoch_ read).
  const std::uint64_t epoch = stop_epoch_;
  client_->SendRequestAsync(
      // Bound the levels requested (0 == "all frames"); a cooperative adapter then
      // caps transfer, and ParseStackFrames caps again for hostile adapters.
      "stackTrace", dap_protocol::MakeStackTraceArguments(thread_id, 0, 10000),
      [this, stop, epoch](const dap_protocol::DapResponse& response) {
        if (!response.success) {
          return;
        }
        if (state_ != State::Stopped || epoch != stop_epoch_) {
          return;  // resumed / superseded before the stack resolved
        }
        // on_stopped is guaranteed non-null by the guard above: callbacks_ is set
        // once at session creation and never reassigned during the session.
        callbacks_.on_stopped(stop, dap_protocol::ParseStackFrames(response.body));
      });
}

void DebugSession::ResolveFocusThreadForStop(std::uint64_t epoch) {
  RequestThreads([this, epoch](std::vector<dap_protocol::DapThread> threads) {
    // Drop the reply if a resume or a newer stop superseded this one.
    if (epoch != stop_epoch_ || state_ != State::Stopped) {
      return;
    }
    if (threads.empty()) {
      return;  // no thread to focus; leave the (already-set) Stopped state as-is
    }
    const int focus = threads.front().id;
    stopped_thread_id_ = focus;
    last_stop_.thread_id = focus;
    // Now that a concrete thread is known, resolve its stack and (re)publish the
    // thread list to the Call Stack selector.
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

// Execution-control commands (SendResumeRequest / Continue / StepOver / StepIn /
// StepOut / ReverseContinue / StepBack / Pause / Restart) live in the companion
// TU DebugSessionExecution.cpp to keep this TU within the debug code-line budget.

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
    // Also require we are still Stopped: a `continued` event moves us to Running
    // WITHOUT bumping stop_epoch_ (see RequestStackTrace), so the epoch check alone
    // would let a late thread-list response repopulate the Call Stack selector
    // after a full resume. Mirror RequestStackTrace's combined guard.
    if (epoch != stop_epoch_ || state_ != State::Stopped) {
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
  last_stop_.thread_id = thread_id;
  const dap_protocol::DapStoppedEvent stop = last_stop_;
  // Bump the stop epoch so a still-in-flight stackTrace from the PREVIOUS thread is
  // dropped instead of projecting the wrong thread's frames — this is exactly the
  // "superseded by a thread switch" case RequestStackTrace's epoch check documents,
  // which only holds if the switch actually advances the epoch (mirrors Reactivate).
  ++stop_epoch_;
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
