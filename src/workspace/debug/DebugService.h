#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "util/Generation.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugSession.h"
#include "workspace/debug/LaunchConfig.h"
#include "workspace/debug/WorkspaceDapManager.h"

namespace microide::editor {
struct AppliedEdit;  // editor/EditTypes.h
}  // namespace microide::editor

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
    // Drop a pruned session's console channel + close its tab (Phase 10 cleanup),
    // so terminated sessions' consoles do not linger.
    std::function<void(int session_id)> remove_debug_console;
    // Notify on session state changes (drives status text / redraw).
    std::function<void(DebugSession::State state)> notify_session_state_changed;
    // Notify when a session reaches a terminal state (Terminated/Failed). `failed`
    // distinguishes an unexpected end (crash / kill / launch rejection) from a clean
    // exit; `reason` carries the teardown message (empty for a clean exit). Drives
    // the control-channel `terminated` broadcast for *every* end, not just clean DAP
    // termination — so an observer is never stranded when an adapter crashes.
    std::function<void(int session_id, bool failed, const std::string& reason)>
        notify_session_terminated;
    // Two-phase stop reporting for push observers (the control channel). Fired
    // for the active session only. `notify_stop_began` lands the instant the
    // adapter halts, carrying the real reason/thread before frames resolve;
    // `notify_stop_resolved` fires once `ProjectStop` has rebuilt the execution
    // view (file/line/frames populated).
    std::function<void(const std::string& reason, int thread_id)> notify_stop_began;
    std::function<void()> notify_stop_resolved;
    std::function<void()> request_chrome_redraw;
    // Repaint the right-side debug dock (Call Stack / Variables / Watch /
    // Breakpoints). DAP responses arrive asynchronously, so every handler that
    // mutates a debug model must invalidate the dock it paints into.
    std::function<void()> request_debug_pane_redraw;
    // Repaint the editor gutter after breakpoint verification reflects back.
    std::function<void()> request_editor_redraw;
    // Re-run editor hover resolution on the next paint. Needed when an async
    // hover-eval value lands while the cursor is still: the redraw alone repaints
    // but does not re-derive the popup unless a hover refresh is queued.
    std::function<void()> queue_editor_hover_refresh;
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
  // Tear down every live session (Phase 10). Blocks until all adapter I/O threads
  // join; resets the project-shared adapter state. No-op when none are active.
  void StopAllDebugging();

  // Re-send `setBreakpoints` for one file to the active session (no-op when no
  // session is active). Used when the user toggles a breakpoint mid-session.
  void ResendBreakpointsForFile(const std::filesystem::path& path);

  // Shift stored line breakpoints in `path` to follow an editor edit (so a line
  // inserted/removed above a breakpoint keeps it on its statement, VSCode-style),
  // rebuild the Breakpoints panel, and live re-send to the active session. Cheap
  // no-op when the file has no breakpoints. Returns true when anything moved (so
  // the caller can repaint the gutter).
  bool ShiftBreakpointsForAppliedEdit(const std::filesystem::path& path,
                                      const editor::AppliedEdit& edit);

  // Execution control (Phase 3). No-ops when no session is active; the session
  // itself guards on the correct state (Stopped for continue/step, Running for
  // pause).
  void Continue();
  void StepOver();
  void StepIn();
  void StepOut();
  void Pause();
  // Reverse execution. No-ops when no session is active or the adapter lacks
  // `supportsStepBack` (the session guards the capability). Mirrors Continue/Step.
  void ReverseContinue();
  void StepBack();
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
  // Set or clear (nullopt) a per-filter exception condition and live re-send
  // `setExceptionBreakpoints` (gated at the wire on the adapter's
  // supportsExceptionFilterOptions + the filter's supportsCondition). No-op when the
  // filter id is not advertised.
  void SetExceptionFilterCondition(const std::string& filter_id,
                                   std::optional<std::string> condition);

  // Function (symbol) breakpoints. Each mutates the per-project
  // FunctionBreakpointStore, rebuilds the Breakpoints panel, and live re-sends
  // `setFunctionBreakpoints` to the active session (gated on
  // supportsFunctionBreakpoints). Add is a no-op for an empty/duplicate name.
  void AddFunctionBreakpoint(std::string name);
  void RemoveFunctionBreakpoint(std::size_t index);
  void ToggleFunctionBreakpointEnabled(std::size_t index);
  void SetFunctionBreakpointCondition(std::size_t index, std::optional<std::string> condition);
  // Name-keyed variants for the command line / control channel (resolve the name to
  // an index in the FunctionBreakpointStore). No-op when the name is not found.
  void RemoveFunctionBreakpointByName(const std::string& name);
  void ToggleFunctionBreakpointByName(const std::string& name);
  void SetFunctionBreakpointConditionByName(const std::string& name,
                                            std::optional<std::string> condition);

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
  // Whether the active session's adapter advertises `supportsStepBack` (gates the
  // reverse-execution commands + their toolbar buttons).
  bool SupportsStepBack() const;

  // Debug-console REPL (Phase 9). Evaluate `expression` against the active
  // session's focused frame (frame 0 when running) with `context:"repl"`, echoing
  // the input and the (async) result into the active session's console channel and
  // surfacing it. Returns false when there is no active session or the expression
  // is empty (nothing was sent).
  bool EvaluateRepl(const std::string& expression);

  bool IsSessionActive() const;
  // The active (UI-projected) session's id (0 when none) and its prebuilt console
  // label. Used to surface the active session's output channel on demand.
  int ActiveSessionId() const;
  std::string ActiveSessionLabel() const;
  DebugSession::State SessionState() const;
  std::string LastError() const;

  // True when the active session has a DAP request in flight (or an undrained
  // response). The idle loop polls on a short interval while this holds so async
  // responses (scopes/variables/evaluate) are applied promptly instead of waiting
  // on a fully-blocking event wait.
  bool HasInFlightDapWork() const;

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
  // Issue one bounded `variables` page request on the active session and feed the
  // result back into `model`, guarded by `generation`.
  //
  // The Variables and Watch panes need exactly this and had a copy each. The
  // guard is the reason it must not be two copies: the adapter recycles
  // variablesReference values, so a page that lands after the generation moved on
  // would attach children to an unrelated node. So would forgetting to re-invoke
  // for the cascade the apply returns — applying a page can auto-expand
  // remembered descendants, and dropping their fetches strands them on
  // "loading…" forever, which is a bug the watch copy had and the variables copy
  // did not.
  //
  // Templated on the model because DebugVariablesModel and DebugWatchModel are
  // separate types with the same MarkChildrenError/ApplyVariables surface. Both
  // instantiations live in DebugServiceVariables.cpp, which is where it is
  // defined.
  template <typename Model>
  void FetchTreeChildrenPage(Model ProjectWorkspaceState::*model_member,
                             util::Generation& generation,
                             int reference,
                             int start,
                             int count);
  void FetchVariablesPage(int reference, int start, int count);
  void FetchWatchChildren(int reference, int start, int count);
  // Project a stop (call stack + focused frame) of the active session into the
  // shared transient views: build the execution view, focus the top frame, surface
  // the Call Stack panel, and jump the editor to the top frame.
  void ProjectStop(const dap_protocol::DapStoppedEvent& stop,
                   const std::vector<dap_protocol::DapStackFrame>& frames);
  // Rebuild the session-selector rows on `debug_execution` from the manager's live
  // sessions + active id (prebuilt display strings; survives resume).
  void SyncSessionsPanel();
  // Rebuild the Breakpoints panel + live re-send function breakpoints to the active
  // session + request a redraw. Shared by the function-breakpoint mutators.
  void ResendFunctionBreakpointsAndSync();
  // Same shape for exception filters: rebuild the panel + live re-send exception
  // filters to the active session + request a redraw. Shared by the exception-filter
  // toggle/condition mutators.
  void ResendExceptionFiltersAndSync();
  // Drop all transient debug view models (execution / variables / hover / watch
  // results) and request redraws. Shared by stop / resume / restart / switch.
  void ClearTransientDebugViews();
  // Reset the project-shared, adapter-owned breakpoint state: line + function
  // breakpoint verification and the advertised exception filters. Called when the
  // last/every session goes away so a fresh session re-verifies and re-advertises
  // from scratch (gutter reads unverified until then).
  void ResetAdapterBreakpointState();
  // Append one text line to a session's console channel via `append_console_output`
  // (wraps the text in a synthetic `console`-category output event). Used by the
  // REPL to echo input and results.
  void AppendConsoleLine(int session_id, const std::string& label, const std::string& text);

  WorkspaceContext* context_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;
  // Remembered for the terminate+relaunch restart fallback.
  LaunchConfig last_launch_config_{};
  std::string last_cwd_;
  // Generation guards for async stale-state. DAP responses arrive a frame or more
  // after their request; by then the user may have switched frame/thread/session
  // or the adapter may have resumed and re-stopped, and the adapter recycles
  // `variablesReference` values across stops. Each generation is bumped at every
  // invalidation point and captured into the dispatching callback; a response whose
  // captured generation no longer matches is dropped instead of applied to the
  // wrong model. `frame_generation_` covers scopes/variables/setVariable (bumped on
  // frame focus + every transient-view clear); `watch_generation_` covers watch
  // evaluates (bumped on each evaluation pass + clear). This generalizes the
  // pattern DebugHoverModel already uses for hover-eval.
  util::Generation frame_generation_;
  util::Generation watch_generation_;
};

}  // namespace microide::workspace
