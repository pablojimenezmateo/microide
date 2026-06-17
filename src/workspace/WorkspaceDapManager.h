#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "platform/SubprocessSandbox.h"
#include "workspace/DebugSession.h"
#include "workspace/LaunchConfig.h"

namespace microide::workspace {

// Per-project registry of contributed debug adapters (keyed by adapter `type`,
// mirroring how LspManager keys servers by language id) plus ownership of the
// active debug session. A debug session is transient — created on "Start
// Debugging", torn down on terminate — unlike an LSP server which is long-lived;
// so the manager stores adapter *definitions* and hands a command+sandbox to the
// session it spawns.
//
// Phase 1 supports a single active session. Phase 7 generalizes to N concurrent
// sessions with an active-session switcher; the per-type registry already
// supports that without change.
class DapManager {
 public:
  DapManager();
  ~DapManager();
  DapManager(const DapManager&) = delete;
  DapManager& operator=(const DapManager&) = delete;

  // SDL event type used to wake the main loop when adapter messages arrive.
  void SetWakeEventType(Uint32 event_type);

  // Register a debug adapter definition for `type`. Idempotent: re-registering
  // the same type with the same command/sandbox is a no-op. Re-registering with
  // different params replaces the definition (a live session keeps running on
  // the old command until it terminates).
  void RegisterAdapter(const std::string& type, const std::vector<std::string>& command,
                       const platform::SubprocessSandbox& sandbox = {});
  // Drop adapter definitions whose type is not in `types` (plugin-reload reconcile).
  void RetainAdaptersIn(const std::unordered_set<std::string>& types);

  bool HasAdapter(const std::string& type) const;
  bool HasRegisteredAdapters() const;
  // Registered adapter type ids (unordered). Used to build a default launch
  // config until per-project launch-config selection lands in a later phase.
  std::vector<std::string> AdapterTypes() const;

  // Begin a debug session for `config`. Resolves `config.type` to a registered
  // adapter, spawns it, and drives the lifecycle, forwarding events through
  // `callbacks`. Returns false (and sets LastError) when the type is unknown or
  // the adapter cannot be spawned. A previously active session is stopped first.
  bool StartSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                    const std::string& cwd = {});

  DebugSession* ActiveSession() { return session_.get(); }
  const DebugSession* ActiveSession() const { return session_.get(); }
  bool HasActiveSession() const { return session_ != nullptr; }

  // Request graceful teardown of the active session (terminate/disconnect). The
  // session object is retained until ClearSession()/StartSession() so terminal
  // state and the last error stay queryable for the UI.
  void StopActiveSession();
  // Drop the active session object outright (blocking shutdown if still running).
  void ClearSession();

  const std::string& LastError() const { return last_error_; }

  // Call from the main thread each frame to dispatch pending callbacks/events.
  void DrainCallbacks();

  // Begin background shutdown without blocking; then ShutdownAll waits.
  void BeginShutdownAll();
  void ShutdownAll();

 private:
  struct AdapterEntry {
    std::vector<std::string> command;
    platform::SubprocessSandbox sandbox;
  };

  Uint32 wake_event_type_ = 0;
  std::unordered_map<std::string, AdapterEntry> adapters_;
  std::unique_ptr<DebugSession> session_;
  std::string last_error_;
};

}  // namespace microide::workspace
