#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
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
    // Repaint the editor gutter after breakpoint verification reflects back.
    std::function<void()> request_editor_redraw;
    // Open + focus the active editor at a stopped frame (0-based line). Used on
    // each stop to jump to the top frame, and on a Call Stack row click.
    std::function<void(const std::filesystem::path& path, std::size_t line)> focus_source_location;
    // Surface the structured Call Stack panel (select + focus its tab) on the
    // first stop of a session.
    std::function<void()> show_call_stack_panel;
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

  // Re-send `setBreakpoints` for one file to the active session (no-op when no
  // session is active). Used when the user toggles a breakpoint mid-session.
  void ResendBreakpointsForFile(const std::filesystem::path& path);

  // Execution control (Phase 3). No-ops when no session is active; the session
  // itself guards on the correct state (Stopped for continue/step, Running for
  // pause).
  void Continue();
  void StepOver();
  void StepIn();
  void StepOut();
  void Pause();

  // Variables panel (Phase 4). Focus a frame → fetch its scopes (and auto-expand
  // the first one); toggle a tree row → lazily fetch/collapse its children;
  // begin/commit/cancel an inline value edit → DAP `setVariable` (gated on the
  // adapter capability). All are no-ops without an active session. FocusFrame is
  // also driven internally on each stop for the top frame.
  void FocusFrame(int frame_id);
  void ToggleVariableRow(std::size_t row);
  void BeginVariableEdit(std::size_t row);
  void CommitVariableEdit();
  void CancelVariableEdit();

  // Hover-to-inspect (Phase 5). Evaluate `expression` in `frame_id`'s scope with
  // `context:"hover"`, writing the result into `debug_hover` and requesting an
  // editor redraw so the (synchronous) hover resolver re-resolves into a cache
  // hit. Deduplicates against the in-flight/cached query, so the chatty per-frame
  // hover trigger issues at most one request per (frame, expression). No-op
  // without an active session.
  void EvaluateHover(int frame_id, const std::string& expression);
  // Whether the active session's adapter advertises `supportsEvaluateForHovers`.
  bool SupportsEvaluateForHovers() const;

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
