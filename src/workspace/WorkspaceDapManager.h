#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "platform/SubprocessSandbox.h"
#include "workspace/DebugSession.h"
#include "workspace/LaunchConfig.h"

namespace microide::workspace {

// A live session's identity for the session-switcher UI. `name` is the launch
// config's name (falling back to its adapter type); `attention` marks a
// background session that has paused but not yet been viewed.
struct DapSessionInfo {
  int id = 0;
  std::string name;
  DebugSession::State state = DebugSession::State::Inactive;
  bool attention = false;
};

// A session dropped by PruneTerminated, captured before the session object is
// destroyed so the caller can finish console bookkeeping. `failed` marks a
// non-clean end (crash / kill / launch rejection); `error` is its teardown reason.
struct PrunedSession {
  int id = 0;
  bool failed = false;
  std::string console_label;
  std::string error;
};

// Per-project registry of contributed debug adapters (keyed by adapter `type`,
// mirroring how LspManager keys servers by language id) plus ownership of the
// live debug sessions. A debug session is transient — created on "Start
// Debugging", torn down on terminate — unlike an LSP server which is long-lived;
// so the manager stores adapter *definitions* and hands a command+sandbox to the
// session it spawns.
//
// Phase 8 holds N concurrent sessions with one *active* session (the one the UI
// projects). Each session carries a stable monotonic id; routing is by
// originating session. The per-type adapter registry is shared across sessions.
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

  // A registered adapter's type plus the argv used to spawn it. Surfaced to the
  // control channel's `adapters` query so a headless driver sees what runs.
  struct AdapterInfo {
    std::string type;
    std::vector<std::string> command;
  };
  std::vector<AdapterInfo> AdapterDetails() const;

  // Begin a new debug session for `config` and make it active. Resolves
  // `config.type` to a registered adapter, spawns it, and drives the lifecycle.
  // The session's stable id is assigned first, then `make_callbacks(id)` builds
  // the callbacks bound to it (so events can be routed by originating session).
  // Returns the new session id, or 0 (LastError populated) when the type is
  // unknown or the adapter cannot be spawned. Existing sessions are untouched.
  int StartSession(const LaunchConfig& config,
                   std::function<DebugSession::Callbacks(int id)> make_callbacks,
                   const std::string& cwd = {});
  // Convenience overload for callers that do not need the session id bound into
  // their callbacks (tests, simple call sites): the same callbacks are used
  // regardless of the assigned id.
  int StartSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                   const std::string& cwd = {});
  // Restart fallback: drop the active session in place, then start a fresh one
  // (so a terminate+relaunch restart does not leave a second session row).
  int ReplaceActiveSession(const LaunchConfig& config,
                           std::function<DebugSession::Callbacks(int id)> make_callbacks,
                           const std::string& cwd = {});
  int ReplaceActiveSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                           const std::string& cwd = {});

  DebugSession* ActiveSession();
  const DebugSession* ActiveSession() const;
  bool HasActiveSession() const { return ActiveSession() != nullptr; }
  int ActiveSessionId() const { return active_session_id_; }
  // Make `id` the active session (no-op if unknown). Does not touch the session.
  void SetActiveSession(int id);
  DebugSession* SessionById(int id);
  std::size_t SessionCount() const { return sessions_.size(); }
  // Mark/clear the "paused in the background" attention flag for a session.
  void SetSessionAttention(int id, bool attention);
  // Live sessions in stable creation order (drives the session-switcher rows).
  std::vector<DapSessionInfo> Sessions() const;

  // Request graceful teardown of the active session (terminate/disconnect). The
  // session object is retained until a later prune so terminal state stays
  // queryable; PruneTerminated() drops it once its I/O thread has joined.
  void StopActiveSession();
  // Drop the active session object outright (blocking shutdown if still running),
  // repointing the active session to the most recent survivor.
  void ClearSession();
  // Reconcile sessions whose adapter process has exited WITHOUT a DAP
  // `terminated`/`exited` event (crash / external kill / RLIMIT_AS cap). Each such
  // session is forced to a terminal state so the following PruneTerminated() can
  // reap it; otherwise it would linger forever as a zombie row. Returns true when
  // any session transitioned (the caller re-syncs the UI). Call before
  // PruneTerminated each frame.
  bool ReapExitedSessions();
  // Drop sessions whose adapter has terminated/failed *and* whose I/O thread has
  // joined (so we never destroy a session inside its own callback). Repoints the
  // active session when it was the one removed. Returns one entry per removed
  // session (empty when nothing changed) so the caller can re-sync the UI and decide
  // each console's fate — a failed session's console is kept (and annotated) so the
  // crash stays inspectable, a clean one's is dropped.
  std::vector<PrunedSession> PruneTerminated();

  const std::string& LastError() const { return last_error_; }

  // Call from the main thread each frame to dispatch pending callbacks/events
  // for *every* live session.
  void DrainCallbacks();

  // Begin background shutdown without blocking; then ShutdownAll waits.
  void BeginShutdownAll();
  void ShutdownAll();

 private:
  struct AdapterEntry {
    std::vector<std::string> command;
    platform::SubprocessSandbox sandbox;
  };
  struct SessionEntry {
    int id = 0;
    bool attention = false;
    std::unique_ptr<DebugSession> session;
  };

  // Index of the entry whose id == active_session_id_, or sessions_.size() if none.
  std::size_t ActiveIndex() const;
  // Drop the active entry (blocking shutdown), repointing active to the survivor.
  void ClearActiveEntry();

  Uint32 wake_event_type_ = 0;
  std::unordered_map<std::string, AdapterEntry> adapters_;
  std::vector<SessionEntry> sessions_;
  int active_session_id_ = 0;
  int next_session_id_ = 1;
  std::string last_error_;
};

}  // namespace microide::workspace
