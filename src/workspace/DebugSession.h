#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <filesystem>

#include "editor/BreakpointStore.h"
#include "platform/SubprocessSandbox.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceDapClient.h"

namespace microide::workspace {

// Drives one debug adapter connection through its DAP lifecycle on top of a
// DapClient. The DapClient owns the transport and the initialize handshake;
// DebugSession owns the higher-level sequencing the spec requires:
//
//   spawn -> initialize -> launch/attach -> (initialized event) ->
//   configurationDone -> running -> terminated
//
// Phase 1 implements the lifecycle and streams `output` events to the console.
// Breakpoints (Phase 2), execution control / `stopped` (Phase 3), variables
// (Phase 4) and the rest layer on through the same client + event callback.
class DebugSession {
 public:
  enum class State {
    Inactive,      // not started
    Initializing,  // adapter spawned; initialize handshake + launch in flight
    Configuring,   // `initialized` event seen; configurationDone in flight
    Running,       // configured; program executing
    Stopped,       // paused at a breakpoint/step (populated from Phase 3)
    Terminated,    // adapter reported terminated/exited, or we disconnected
    Failed,        // adapter failed to spawn, or launch/attach was rejected
  };

  struct Callbacks {
    // Adapter `output` events (stdout/stderr/console) destined for the console.
    std::function<void(const dap_protocol::DapOutputEvent& output)> on_output;
    // Fired whenever CurrentState() changes (drives status text / redraw).
    std::function<void(State state)> on_state_changed;
    // Every adapter event verbatim, for phases that react to stopped/continued/
    // breakpoint/thread events. Delivered after the built-in lifecycle handling.
    std::function<void(const std::string& event, const util::JsonValue& body)> on_event;
    // Fired the instant a `stopped` event arrives, carrying the real reason +
    // thread straight from the DAP event — before the async `stackTrace` resolves
    // frames. Lets push-based observers (the control channel) report the halt
    // within ms even while a slow adapter (e.g. gdb indexing DWARF) resolves the
    // stack. The populated follow-up arrives via `on_stopped`.
    std::function<void(const dap_protocol::DapStoppedEvent& stop)> on_stop_began;
    // Fired after a `stopped` event's `stackTrace` response resolves: the host
    // gets the stop metadata plus the focused thread's frames (top frame first)
    // so it can drive the execution-line highlight and Call Stack panel.
    std::function<void(const dap_protocol::DapStoppedEvent& stop,
                       const std::vector<dap_protocol::DapStackFrame>& frames)>
        on_stopped;
    // Fired when execution resumes (a continue/step request is sent, or the
    // adapter emits `continued`) so the host clears the execution-line + stack.
    std::function<void()> on_resumed;
    // Fired with the adapter's full thread list (Phase 7 multi-thread). Resolved
    // a beat after `on_stopped` so the Call Stack panel can offer a thread
    // selector without delaying the stack/highlight on the stopped thread.
    std::function<void(const std::vector<dap_protocol::DapThread>& threads)> on_threads;
    // Pulled when the adapter is ready for configuration (the `initialized`
    // event) and on every live re-send, to get the breakpoints to install.
    std::function<std::vector<editor::BreakpointStore::FileBreakpoints>()> breakpoint_provider;
    // Pushed with the adapter's verified/rejected breakpoints for one file,
    // positional to the setBreakpoints request, so the host can reflect state.
    std::function<void(const std::filesystem::path& path,
                       const std::vector<dap_protocol::DapBreakpoint>& breakpoints)>
        on_breakpoints_verified;
    // Pushed with the adapter's advertised exception-breakpoint filters when the
    // session initializes (Phase 7), so the host can populate the Breakpoints tab
    // and seed the enabled set from each filter's default.
    std::function<void(const std::vector<dap_protocol::DapExceptionFilter>& filters)>
        on_exception_filters_available;
    // Pulled at `initialized` and on every live re-send to get the enabled
    // exception-filter ids to install via setExceptionBreakpoints.
    std::function<std::vector<std::string>()> exception_filter_provider;
  };

  DebugSession();
  // Test seam: adopt a pre-built client (e.g. one in EnableTestStubMode()).
  explicit DebugSession(std::unique_ptr<DapClient> client);
  ~DebugSession();
  DebugSession(const DebugSession&) = delete;
  DebugSession& operator=(const DebugSession&) = delete;

  void SetWakeEventType(Uint32 event_type);
  void SetCallbacks(Callbacks callbacks);

  // Spawn `command` and begin driving the lifecycle for `config`. Returns false
  // only when the adapter process cannot be spawned (state becomes Failed).
  bool Start(const std::vector<std::string>& command, const LaunchConfig& config,
             const std::string& cwd = {}, const platform::SubprocessSandbox& sandbox = {});

  State CurrentState() const { return state_; }
  // Active = the session is doing something the UI should reflect.
  bool IsActive() const;
  const std::string& LastError() const { return last_error_; }
  const LaunchConfig& Config() const { return config_; }

  // Drain the underlying client (delivers responses/events on the main thread).
  void DrainCallbacks();

  // Request graceful teardown: `terminate` when the adapter supports it (lets a
  // launched debuggee shut down cleanly), otherwise `disconnect`.
  void RequestStop();

  // Execution control (Phase 3). continue/step are valid only while Stopped and
  // target the most recently stopped thread; each optimistically resumes (fires
  // on_resumed + State::Running) for snappy UX — the next `stopped` repopulates.
  // Pause is valid only while Running: it resolves a thread via `threads` and
  // sends `pause` for the first one.
  void Continue();
  void StepOver();  // DAP `next`
  void StepIn();
  void StepOut();
  void Pause();

  // Restart the active session in place via the DAP `restart` request (Phase 7).
  // Only valid when the adapter advertises `supportsRestartRequest`; callers
  // without the capability terminate + relaunch at the service layer instead.
  // Re-arms the configurationDone guard so a re-emitted `initialized` re-installs
  // breakpoints, and optimistically resumes so the UI clears at once.
  void Restart();

  // Threads (Phase 7 multi-thread). Fetch the adapter's full thread list; the
  // callback fires on the main thread when the response arrives. No-op until the
  // adapter is initialized.
  void RequestThreads(std::function<void(std::vector<dap_protocol::DapThread>)> callback);
  // Re-resolve the call stack for `thread_id` while Stopped and hand the frames
  // up through `on_stopped` (with the cached stop reason) so the host re-focuses
  // the picked thread. Used by the Call Stack thread selector.
  void SwitchThread(int thread_id);

  // Re-project this session's current stop through `on_stopped` + `on_threads`
  // (Phase 8 multi-session). Used when the user switches the active session back
  // to this one: it rebuilds the shared execution view from the session's
  // retained last stop. No-op unless the session is Stopped.
  void Reactivate();

  // Variables inspection (Phase 4). Request/response style with inline callbacks
  // (these are not lifecycle events, so they stay off the Callbacks struct — the
  // same shape as Pause()'s `threads` round-trip). Callbacks fire on the main
  // thread when the response arrives; no-ops until the adapter is initialized.
  // Fetch the scopes for one focused frame.
  void RequestScopes(int frame_id,
                     std::function<void(std::vector<dap_protocol::DapScope>)> callback);
  // Fetch a bounded page of children of a structured variable/scope
  // (variablesReference > 0): `count` children starting at `start` (DAP variable
  // paging). Bounding the request is what prevents the adapter from enumerating a
  // garbage / billions-long container in one shot. The callback's `ok` is false on
  // an adapter error / send failure / not-yet-initialized so the caller can clear
  // the loading state instead of waiting forever.
  void RequestVariables(int variables_reference, int start, int count,
                        std::function<void(bool ok, std::vector<dap_protocol::DapVariable>)> callback);
  // Mutate a variable's value. Gated on `supportsSetVariable`; when unsupported the
  // callback fires immediately with ok=false. On success the adapter echoes the
  // (possibly normalized) value/type/reference.
  void SetVariable(int variables_reference, const std::string& name, const std::string& value,
                   std::function<void(bool ok, dap_protocol::DapSetVariableResult)> callback);

  // Evaluate an expression in a frame's scope (Phase 5: hover-to-inspect). Gated
  // on `supportsEvaluateForHovers` for `context:"hover"`; when unsupported the
  // callback fires immediately with ok=false. Same inline request/response shape
  // as RequestScopes — fires on the main thread when the response arrives.
  void RequestEvaluate(const std::string& expression, int frame_id, const std::string& context,
                       std::function<void(bool ok, dap_protocol::DapEvaluateResult)> callback);

  // Re-send `setBreakpoints` for one file while the session is live (e.g. the
  // user toggled a breakpoint after launch). Pulls the current snapshot from
  // `breakpoint_provider`; sends an empty list to clear a file. No-op until the
  // adapter is initialized.
  void ResendBreakpointsForFile(const std::filesystem::path& path);

  // Re-send `setExceptionBreakpoints` with the current enabled filter set while
  // the session is live (e.g. the user toggled a filter). Pulls the enabled ids
  // from `exception_filter_provider` and intersects them with the adapter's
  // advertised filters. No-op until the adapter is initialized.
  void ResendExceptionFilters();

  DapClient& Client() { return *client_; }
  const DapClient& Client() const { return *client_; }

 private:
  void HandleEvent(const std::string& event, const util::JsonValue& body);
  void RequestStackTrace(const dap_protocol::DapStoppedEvent& stop);
  // Fetch the thread list and forward it through `on_threads` (augments the
  // Call Stack panel a beat after the stack itself resolves; keeps the stop fast).
  void RequestThreadsForStop();
  // Shared by continue/step: guard on Stopped, send `command{threadId}`, then
  // optimistically resume.
  void SendResumeRequest(const char* command);
  void SendLaunchRequest();
  void SendAllBreakpoints();
  void SendBreakpointsForFile(const editor::BreakpointStore::FileBreakpoints& file);
  void SendExceptionFilters();
  void SendConfigurationDone();
  // Send gdb-specific value-formatting ceilings (print elements/repeats,
  // max-value-size) so an uninitialized/corrupt container cannot drive gdb into
  // unbounded formatting. No-op for non-gdb adapters. Best-effort (errors ignored).
  void SendDebuggerValueLimits();
  void SetState(State state);

  std::unique_ptr<DapClient> client_;
  Callbacks callbacks_{};
  LaunchConfig config_{};
  State state_ = State::Inactive;
  std::string last_error_;
  bool launch_sent_ = false;
  bool configuration_done_sent_ = false;
  // True when the adapter command looks like gdb (`gdb --interpreter=dap`); gates
  // the gdb-specific value-formatting limits.
  bool is_gdb_adapter_ = false;
  // Thread id from the most recent `stopped` event; the target for stackTrace
  // and continue/step requests.
  int stopped_thread_id_ = 0;
  // The most recent `stopped` event, retained so SwitchThread can re-emit
  // on_stopped with the original stop reason for a different thread's frames.
  dap_protocol::DapStoppedEvent last_stop_{};
};

// Stable name for a session state (status text, tracing, tests).
const char* DebugSessionStateName(DebugSession::State state);

}  // namespace microide::workspace
