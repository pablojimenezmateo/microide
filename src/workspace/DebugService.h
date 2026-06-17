#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>

#include "workspace/DapProtocol.h"
#include "workspace/DebugSession.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceDapManager.h"

namespace microide::workspace {

struct WorkspaceContext;
struct ProjectWorkspaceState;

// Host-owned home for the DAP glue, mirroring LspService: per-project adapter
// management, debug-session lifecycle, and console-output streaming. WorkspaceShell
// keeps thin forwarders; render/menu/plugin TUs stay decoupled. Shell wiring is
// injected through the narrow Operations seam.
//
// The "Start Debugging" affordance is gated on the `debug.enabled` setting at the
// action layer; this service is independent of the toggle (the protocol client
// and session lifecycle run the same way regardless).
class DebugService {
 public:
  struct Operations {
    // Stream a DAP `output` event to the debug console (an output channel).
    std::function<void(const dap_protocol::DapOutputEvent& output)> append_console_output;
    // Notify on session state changes (drives status text / redraw).
    std::function<void(DebugSession::State state)> notify_session_state_changed;
    std::function<void()> request_chrome_redraw;
    std::function<void()> request_bottom_panel_redraw;
  };

  DebugService() = default;

  void Configure(WorkspaceContext& context, Operations operations);
  void SetWakeEventType(Uint32 event_type);

  // Per-project adapter manager access and main-thread callback pump.
  DapManager& CurrentDapManager();
  const DapManager& CurrentDapManager() const;
  DapManager& EnsureProjectDapManager(ProjectWorkspaceState& state);
  void ConsumeDapCallbacks();

  // Begin a debug session for `config`. Wires the configured Operations into the
  // session's callbacks. Returns false (LastError populated) when the adapter
  // type is unknown or the adapter cannot be spawned.
  bool StartDebugging(const LaunchConfig& config, const std::string& cwd = {});
  // Request graceful teardown of the active session.
  void StopDebugging();

  bool IsSessionActive() const;
  DebugSession::State SessionState() const;
  std::string LastError() const;

 private:
  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;
};

}  // namespace microide::workspace
