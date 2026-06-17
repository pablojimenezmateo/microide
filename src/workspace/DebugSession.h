#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

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

  DapClient& Client() { return *client_; }
  const DapClient& Client() const { return *client_; }

 private:
  void HandleEvent(const std::string& event, const util::JsonValue& body);
  void SendLaunchRequest();
  void SendConfigurationDone();
  void SetState(State state);

  std::unique_ptr<DapClient> client_;
  Callbacks callbacks_{};
  LaunchConfig config_{};
  State state_ = State::Inactive;
  std::string last_error_;
  bool launch_sent_ = false;
  bool configuration_done_sent_ = false;
};

// Stable name for a session state (status text, tracing, tests).
const char* DebugSessionStateName(DebugSession::State state);

}  // namespace microide::workspace
