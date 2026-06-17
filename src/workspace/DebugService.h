#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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
    // Stream a DAP `output` event to a session's debug console (one output channel
    // per session, keyed by session id; `label` names the channel/tab).
    std::function<void(int session_id, const std::string& label,
                       const dap_protocol::DapOutputEvent& output)>
        append_console_output;
    // Surface (open + select) a session's console channel. Called on session start
    // and on a session switch so the console follows the active session.
    std::function<void(int session_id, const std::string& label)> show_debug_console;
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
  // Restart the active session (Phase 7). Uses the DAP `restart` request when the
  // adapter advertises `supportsRestartRequest`, otherwise terminates and
  // relaunches with the same config + cwd. No-op when no session is active.
  void Restart();

  // Focus a thread in the Call Stack thread selector (Phase 7 multi-thread):
  // re-resolves the picked thread's frames and re-focuses it. No-op when not
  // stopped / no session.
  void FocusThread(int thread_id);

  // Make `session_id` the active debug session (Phase 8 multi-session): clears the
  // previously-active session's transient view, surfaces the picked session's
  // console, and re-projects its current stop (via DebugSession::Reactivate). The
  // session-switcher command + Call Stack session-row click both route here.
  // No-op for an unknown id.
  void FocusSession(int session_id);
  // Cycle the active session to the next live one (wraps). Used by the no-arg
  // `debug-switch-session` command. No-op with fewer than two sessions.
  void FocusNextSession();
  // Live sessions for the switcher UI (id + prebuilt label + state + attention).
  std::vector<DapSessionInfo> Sessions() const;

  // Breakpoints panel (Phase 7). Toggle one exception-breakpoint filter and live
  // re-send `setExceptionBreakpoints` to the active session (if any). Rebuild the
  // panel's prebuilt rows from the current breakpoints + advertised filters
  // (called by the shell whenever the breakpoint set or filters change).
  void ToggleExceptionFilter(const std::string& filter_id);
  void SyncBreakpointsPanel();

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

  // Watch panel (Phase 6). The expression list is persistent; on each stop (and
  // frame switch) every expression is re-evaluated with `evaluate(context:
  // "watch")` against the focused frame. Add/Edit/Remove mutate the persisted
  // list and re-evaluate immediately when a session is stopped. ToggleWatchRow
  // lazily expands a structured result; the *Edit methods drive inline
  // setVariable on a watched value's child (gated on the adapter capability).
  void EvaluateWatches(int frame_id);
  std::size_t AddWatch(std::string expression);
  void EditWatch(std::size_t index, std::string expression);
  void RemoveWatch(std::size_t index);
  void ToggleWatchRow(std::size_t row);
  void BeginWatchEdit(std::size_t row);
  void CommitWatchEdit();
  void CancelWatchEdit();
  // Focused frame id of the active stop (0 when not stopped / no session).
  int FocusedFrameId() const;

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
  // Build the session callbacks that wire the configured Operations in, bound to
  // the originating session's id + console label so events route correctly. Shared
  // by StartDebugging and the restart relaunch path.
  DebugSession::Callbacks BuildSessionCallbacks(int session_id, std::string session_label);
  // True when `session_id` is the active (UI-projected) session.
  bool IsActiveSession(int session_id) const;
  // Spawn a session for `config` (replacing the active one when `replace`), wire
  // its callbacks, surface its console, and sync the switcher. Returns the new id
  // (0 on failure). Shared by StartDebugging and the restart relaunch fallback.
  int LaunchSession(const LaunchConfig& config, const std::string& cwd, bool replace);
  // Project a stop (call stack + focused frame) of the active session into the
  // shared transient views: build the execution view, focus the top frame, surface
  // the Call Stack panel, and jump the editor to the top frame.
  void ProjectStop(const dap_protocol::DapStoppedEvent& stop,
                   const std::vector<dap_protocol::DapStackFrame>& frames);
  // Rebuild the session-selector rows on `debug_execution` from the manager's live
  // sessions + active id (prebuilt display strings; survives resume).
  void SyncSessionsPanel();
  // Drop all transient debug view models (execution / variables / hover / watch
  // results) and request redraws. Shared by stop / resume / restart / switch.
  void ClearTransientDebugViews();

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;
  // Remembered for the terminate+relaunch restart fallback.
  LaunchConfig last_launch_config_{};
  std::string last_cwd_;
};

}  // namespace microide::workspace
