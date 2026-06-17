# Debugger / DAP Integration

Status: **active, dedicated phase** (promoted from the durable non-goal list on
2026-06-17). This document is the single self-sufficient source of truth for the
debugger effort — a fresh agent on any machine should be able to read this file
and continue without external context.

## Goal

Full interactive debugging in microide, driven through the **Debug Adapter
Protocol (DAP)** so any DAP-speaking debugger (lldb-dap, debugpy, delve, codelldb,
…) works. Target capabilities:

- breakpoints, conditional breakpoints, hit-count breakpoints, logpoints
- step in / over / out, pause, continue, restart
- call stack + thread inspection
- variable inspection **and** mutation (setVariable)
- hover-to-inspect a variable's value in the editor
- watch expressions
- exception breakpoints

UX bar is high (Godot-like): click the line-number gutter to toggle breakpoints,
full-width execution-line highlight, hover popups, dedicated debug panels. All of
it sits behind a master **"Enable debugger"** toggle (see below) so a user who
does not debug sees the editor exactly as it is today.

Priorities, in order: **speed → correctness → low CPU/memory**.

## Core architectural decisions (locked)

1. **Host-owned DAP client, mirroring the LSP client.** The host owns the process,
   protocol, and all UI. Plugins only *contribute adapter definitions* (a future
   `ctx.debug.add{…}`), exactly as they contribute language servers via
   `ctx.lsp.add{…}`. This is required by the hard invariants (rendering is
   host-owned, `lua_State*` lives only behind `plugin/LuaRuntime`, no plugin
   `.cpp` > 800 lines). The LSP client is a near-exact template because DAP shares
   the JSON-over-stdio `Content-Length` framing.
2. **Native launch-config format only.** Per-project breakpoints and launch configs
   persist through `PersistenceService` + `PersistedRecord*` (binary + CRC32C).
   **No `.vscode/launch.json` import** in this effort (hand-rolled / foreign
   formats are banned by the persistence invariants).
3. **Master "Enable debugger" toggle** gates every debug affordance. Default OFF.

## The "Enable debugger" toggle

A boolean setting (working name `debug.enabled`) registered through
`WorkspaceSettingsRegistry` / `SettingsOverlayService`, scoped per user (with the
option of a per-project override later). Semantics:

- **OFF (default):** the editor is byte-for-byte today's behavior. No breakpoint
  gutter interaction, no debug panels, no debug commands in menus/command palette,
  no hover-to-inspect. The DAP/`DebugService` machinery is inert.
- **ON:** debug extras activate — clickable breakpoint gutter, debug panels
  (Console / Call Stack / Variables / Watch / Breakpoints), step commands,
  execution-line highlight, hover-to-inspect.

The toggle is purely a UI/affordance gate; the protocol client and `DebugService`
are independent of it. It is introduced in **Phase 1** (so even "Start Debugging"
is gated) and every later UI element checks it. The check flows through
`RenderViewModelBuilder` so render TUs never read the setting directly.

## Where this plugs into the existing codebase

The LSP subsystem is the template at every layer. Concrete anchors:

| Concern | LSP anchor (template) | DAP counterpart |
| --- | --- | --- |
| Protocol client | `src/workspace/WorkspaceLspClient*.{h,cpp}`, `WorkspaceLspClientInternal.h` | `src/workspace/WorkspaceDapClient*` (Phase 0 ✔) |
| Wire-type mapping | `src/workspace/LspProtocol.{h,cpp}` | `src/workspace/DapProtocol.{h,cpp}` (Phase 0 ✔) |
| Async subprocess transport | `src/platform/AsyncSubprocess.h` (poll + stdout_fd) | reused as-is |
| JSON codec | `src/util/JsonValue.h` | reused as-is |
| Service wired into shell | `src/workspace/LspService.h` (`Configure(ctx, Operations)`, `SetWakeEventType`, `ConsumeLspCallbacks()` drained per frame) | `DebugService` (Phase 1) |
| Per-language/adapter manager | `src/workspace/WorkspaceLspManager.{h,cpp}` | `WorkspaceDapManager` (Phase 1) |
| Plugin contribution | `ctx.lsp.add` → `ContributedLanguageServer` in `src/plugin/PluginHost.h`, parsed in `PluginProviderRegistrationParsers.cpp` | `ctx.debug.add` → `ContributedDebugAdapter` (Phase 1) |
| Gutter click | fold-marker click sub-region in `src/workspace/WorkspaceEditorMouseCoordinator.cpp`; `WorkspaceLayout::VisibleTextGridLineAtY()` | breakpoint toggle (Phase 2) |
| Gutter marker render | `src/editor/DiagnosticsRender.cpp` `DiagnosticGutterMarkerRect` | breakpoint dots (Phase 2) |
| Line decoration / fill | `src/editor/RowDecorationBuilder.h` (`RowFillSpan`), `DecoratedTextGridRenderer.h` | execution-line highlight (Phase 3) |
| Per-frame editor view model | `src/editor/EditorViewModel.h`, built by `RenderViewModelBuilder` | breakpoint marks + execution line (Phase 2/3) |
| Hover | `src/workspace/WorkspaceShellHoverTargets.cpp` / `WorkspaceShellHoverPopup.cpp` (`QueryHover`) | hover-to-inspect (Phase 5) |
| Bottom panel | `PanelContentKind`/`PanelState` in `src/workspace/WorkspaceProjectState.h`; `TerminalPanelService`; `WorkspaceShellRenderBottomPanel.cpp` | debug panels (Phase 1/3/4/6) |
| Persistence | `src/workspace/WorkspacePersistenceFormat.h` (`EncodeProjectSessionRecord`), `PersistenceService` | `PersistedDebugState` (Phase 2) |

## Hard invariants the implementation must respect

Enforced by `tests/ArchitectureInvariantsTests.cpp`:

- `WorkspaceShell.h` ≤ 400 lines, `.cpp` ≤ 600 — push logic into
  `DebugService`/`DebugSession`, keep only thin forwarders in the shell.
- Render TUs consume `RenderViewModelBuilder` view models only; never read
  `current_project_state`, never materialize strings in hot paths.
- Workspace state persists only through `PersistenceService` + `PersistedRecord*`.
- No `platform::RunSubprocess` in workspace `.cpp` (the DAP client uses
  `AsyncSubprocess`, like the LSP client).
- Plugin Lua: `lua_State*` behind `LuaRuntime` only; raise errors via
  `lua_error_util` (never `luaL_error`); no plugin `.cpp` > 800 lines.

Note: a prior `CheckNoDebuggerDapSurface` architecture guard (which banned any
file named `*Dap*` and a set of debugger symbols) was **retired on 2026-06-17**
when DAP was promoted to an active phase. Do not reintroduce it.

## Phased roadmap

Each phase is independently shippable and testable. The reusable test spine is the
Python mock-adapter pattern from `tests/WorkspaceLspClientTests.cpp` /
`tests/WorkspaceDapClientTests.cpp`. Add a fuzz target for every new
`PersistedRecord` decoder.

### Phase 0 — Protocol client core ✅ DONE (2026-06-17)

`DapClient` mirroring `LspClient`, plus `DapProtocol` and a mock-adapter test
harness. No UI. See "Phase 0 — what shipped" below.

### Phase 1 — `DebugService` + `ctx.debug.add` + session lifecycle + debug console + **toggle** ✅ DONE (2026-06-17)

See "Phase 1 — what shipped" below. Summary: `DebugService`/`DapManager`/
`DebugSession` drive the launch lifecycle, `ctx.debug.add` contributes adapters,
the `debug.enabled` toggle and Start/Stop Debugging commands gate the surface, and
`output` events stream to a Debug Console. Two design notes vs. the original plan:

- **Console reuses the output-channel infrastructure.** The Debug Console is a
  `WorkspaceOutputChannels` channel (`debug.console`) rendered through the
  existing `PanelContentKind::Output` path, rather than a new
  `PanelContentKind::Debug` + `DebugConsoleState`. The Output panel is already
  channel-backed, so this reuses proven rendering/tab-strip/scroll code and adds
  no render-TU surface. A dedicated `PanelContentKind::Debug` will arrive when
  Phase 3+ introduces *structured* debug panels (Call Stack / Variables / Watch),
  which are not text output.
- **Launch config is minimal.** `StartDebuggingWithDefaultConfig` launches the
  first registered adapter with an empty-argument `launch` request. Per-project
  launch-config selection + persistence (native `PersistedRecord` format, no
  `.vscode/launch.json`) is deferred; it pairs naturally with breakpoint
  persistence in Phase 2.

Original plan, retained for reference:


- New: `src/workspace/DebugService.{h,cpp}` (mirror `LspService`), `WorkspaceDapManager.{h,cpp}`
  (mirror `WorkspaceLspManager`), `DebugSession.{h,cpp}` (state machine
  Inactive→Initializing→Running→Stopped→Terminated), `LaunchConfig.h`.
- Plugin seam: add `ContributedDebugAdapter` to `src/plugin/PluginHost.h`,
  `ParseDebugAdapterRegistration` in `PluginProviderRegistrationParsers.cpp`, wire
  `ctx.debug.add` through `PluginLuaContextInterop.cpp` + `PluginHostLuaApi.inc` +
  `PluginHostPublicApi.inc`.
- Shell: add `DebugService debug_service_;` + thin forwarders; add
  `ConsumeDapCallbacks()` to the per-frame drain next to `ConsumeLspCallbacks()`.
- UI: `PanelContentKind::Debug` + `DebugConsoleState` (ring buffer, mirror
  `OutputPanelState`) in `WorkspaceProjectState.h`; `DebugConsoleViewModel` via
  `RenderViewModelBuilder`; render in `WorkspaceShellRenderBottomPanel.cpp`.
- **Toggle:** register the `debug.enabled` setting; gate "Start Debugging" and the
  console on it.
- DAP surface: `initialize`(+capabilities), `launch`/`attach`, `configurationDone`
  (gated on `supportsConfigurationDoneRequest`), `output` event, `terminated`/
  `exited`, `terminate`/`disconnect`.
- Sandbox note: debuggers often need ptrace + broad fs access, so the default
  `SubprocessSandbox` for adapters is more permissive than LSP/formatters.
- Tests: `DebugServiceTests.cpp`, `PluginDebugAdapterRegistrationTests.cpp`.

### Phase 2 — Breakpoints (gutter click toggle + persistence + `setBreakpoints`) ✅ DONE (2026-06-17)

See "Phase 2 — what shipped" below. Summary: a clickable gutter toggles
breakpoints (gated on `debug.enabled`), dots render via the editor view model,
breakpoints + launch configs persist in a per-project `debug` record (binary +
CRC32C, fuzzed), `setBreakpoints` is sent per file on launch (and live on toggle)
with verification reflected back, and `ctx.debug.addConfig` lets plugins
contribute launch configs that Start Debugging can target.

Original plan, retained for reference:

- New: `src/workspace/BreakpointStore.{h,cpp}` (per-project path→breakpoints, with
  condition/hit/log fields reserved for Phase 6).
- Gutter click in `WorkspaceEditorMouseCoordinator.cpp` (model on the fold-marker
  sub-region; partition the gutter so it does not collide with fold markers).
- `BreakpointGutterMark` in `EditorViewModel.h`; populate in `RenderViewModelBuilder`;
  paint dots in `EditorViewRenderer.cpp` reusing `DiagnosticGutterMarkerRect`.
- Persistence: `PersistedDebugState` in `WorkspacePersistenceFormat.h` +
  `PersistenceService`/`WorkspacePersistenceBinaryFormat.cpp`/
  `WorkspacePersistenceCoordinatorSession.cpp`; add a fuzz target for
  `DecodeDebugStateRecord`.
- On launch, send `setBreakpoints` per file; reflect verified/unverified.
- DAP surface: `setBreakpoints`, breakpoint verification, `breakpoint` event.

### Phase 3 — Execution control + `stopped` + current-line highlight + Call Stack ✅ DONE (2026-06-17)

See "Phase 3 — what shipped" below. Summary: `stopped` resolves `stackTrace` and
populates a host-owned `DebugExecutionView`; the focused frame paints a full-width
execution-line fill + gutter arrow (cleared on resume) and is mirrored by a
structured Call Stack bottom panel (`PanelContentKind::Debug`) whose rows navigate
on click; bindable execution-control commands (continue / step over / step in /
step out / pause) gate on `debug.enabled` + session state.

Original plan, retained for reference:

- New: `src/workspace/DebugViewModel.h`, `DebugCommands.{h,cpp}` (`debug.continue/
  stepOver/stepIn/stepOut/pause/stop/restart`, bindable).
- On `stopped`: `threads`→`stackTrace`→focus top frame; full-width execution-line
  fill via `RowDecorationBuilder` + gutter arrow; clear on `continued`.
- Call Stack rows in the debug panel (clickable → navigate; store `focused_frame_id`).
- DAP surface: `stopped`, `continued`, `threads`, `stackTrace`, `continue`, `next`,
  `stepIn`, `stepOut`, `pause`.

### Phase 4 — Variables / Scopes panel + `setVariable`

- New: `src/workspace/DebugVariablesModel.{h,cpp}` (lazy tree keyed by
  `variablesReference`, cleared on each stop).
- On frame focus → `scopes`; on expand → `variables` (paged via `start`/`count`);
  inline edit via `SingleLineEditor` → `setVariable`. Row text prebuilt in the view
  model. DAP surface: `scopes`, `variables`, `setVariable`.

### Phase 5 — Hover-to-inspect via `evaluate`

- Add `EditorHoverTarget::Kind::DebugValue` in `WorkspaceShellHoverTargets.cpp`;
  when paused + hovering a token, resolve its range and
  `evaluate(expr, frameId, context:"hover")`, caching like plugin hover; render via
  `WorkspaceShellHoverPopup.cpp`. DAP surface: `evaluate` (`context:"hover"`).

### Phase 6 — Conditional / hit-count / logpoints + watch expressions

- Breakpoint context menu sets condition/hit/log (fields already on `BreakpointStore`;
  gate on the matching capabilities). New `WatchExpressionStore.{h,cpp}` persisted in
  `PersistedDebugState`; Watch panel re-evaluates on each `stopped`
  (`evaluate(context:"watch")`).

### Phase 7 — Polish: multi-thread, multi-session, exception breakpoints, restart

- N concurrent `DebugSession`s with an active-session switcher (each `DapClient`
  owns its own `seq`/`pending_requests`; route events by originating client).
  Thread selector in the call-stack panel. `setExceptionBreakpoints(filters)` from
  `capabilities.exceptionBreakpointFilters`. `restart` with terminate+relaunch
  fallback if `!supportsRestartRequest`. Run the TSAN preset (multi-thread I/O).

## Phase 0 — what shipped (2026-06-17)

Files added:

- `src/workspace/DapProtocol.h` / `DapProtocol.cpp` — DAP envelope encode
  (`MakeRequest`/`MakeResponse`), `ParseResponse`, `ParseCapabilities`, and parsers
  for stopped/output events, threads, stack frames + source, scopes, variables,
  breakpoints, evaluate results. Pure functions; one TU owns the wire mapping.
- `src/workspace/WorkspaceDapClient.h` — public `DapClient`: `SetWakeEventType`,
  `SetEventCallback`, `Start(command, adapter_id, cwd, sandbox)`, `IsRunning/
  IsInitializing/IsInitialized`, `Capabilities()`, `LastError()`, `DrainCallbacks()`,
  `SendRequestAsync(command, args, callback)`, `BeginShutdown/Shutdown`, plus a test
  stub mode (`EnableTestStubMode`, `SetTestRequestHandler`, `InjectTestEvent`).
- `src/workspace/WorkspaceDapClientInternal.h` — `DapClient::Impl`. Mirrors
  `LspClient::Impl`: dedicated I/O thread blocked in `poll()` over stdout + a
  self-pipe wake (`DapReadBuf` buffer with compaction), outbound queue, deferred
  pre-initialize queue, `seq`-keyed `pending_requests`, `ready_callbacks` drained on
  the main thread, `PushWakeEvent()` → `SDL_PushEvent`. DAP specifics: monotonic
  `seq`; envelope `type` switch (response/event/request); responses correlate by
  `request_seq`; initialize handshake parses capabilities and starts the I/O thread;
  adapter-initiated reverse requests get a "not supported" response; shutdown sends
  `disconnect` and waits for its response.
- `src/workspace/WorkspaceDapClient.cpp` — public method implementations.

Key DAP-vs-LSP differences encoded in the client:

- LSP uses JSON-RPC `id`; DAP uses `seq` + `request_seq`.
- LSP distinguishes messages by method/id presence; DAP uses an explicit `type`
  field.
- DAP has no client-sent `initialized` notification — the adapter sends an
  `initialized` *event* (delivered to the event callback for Phase 1's
  `DebugSession` to react to). The client's internal `initialized` flag means "the
  `initialize` response arrived" and gates the outbound queue.

Tests added (registered in `tests/TestMain.cpp`, wired in `CMakeLists.txt`):

- `tests/DapProtocolTests.cpp` — pure encode/decode round-trips (8 cases).
- `tests/WorkspaceDapClientTests.cpp` — Python mock adapter: initialize handshake +
  capabilities, request/response correlation by `seq`, event dispatch on the main
  thread, adapter-exits-before-initialize error handling, shutdown sends
  `disconnect`, and stub-mode request/event flow (6 cases).

Architecture change: retired `CheckNoDebuggerDapSurface` and its fixture/test
(`tests/architecture/WorkspaceCoordinatorArchitectureRules.{h,cpp}`,
`WorkspaceArchitectureRules.{h,cpp}`, `tests/ArchitectureInvariantsTests.cpp`).

## Phase 1 — what shipped (2026-06-17)

Files added:

- `src/workspace/LaunchConfig.h` — native launch/attach config (`name`, `type`,
  `request`, verbatim `arguments`). No `.vscode/launch.json` import.
- `src/workspace/DebugSession.{h,cpp}` — lifecycle state machine
  (`Inactive→Initializing→Configuring→Running→Stopped→Terminated/Failed`) on top of
  a `DapClient`. Drives initialize → launch/attach (queued; the client flushes it
  once the initialize response arrives) → `configurationDone` (gated on
  `supportsConfigurationDoneRequest`) → running; streams `output`; tears down on
  `terminated`/`exited`. `RequestStop` prefers `terminate` over `disconnect` when
  the adapter supports it.
- `src/workspace/WorkspaceDapManager.{h,cpp}` — per-project `DapManager`: adapter
  registry keyed by adapter `type` (mirrors `LspManager`'s per-language servers),
  owning the transient active `DebugSession`. `RegisterAdapter` / `RetainAdaptersIn`
  / `StartSession` / `StopActiveSession` / `DrainCallbacks`.
- `src/workspace/DebugService.{h,cpp}` — shell-facing facade mirroring `LspService`:
  `Configure`, `SetWakeEventType`, `CurrentDapManager`/`EnsureProjectDapManager`,
  `ConsumeDapCallbacks`, `StartDebugging`/`StopDebugging`.

Wiring:

- `ProjectWorkspaceState.dap_manager` (lazy, mirrors `lsp_manager`); ensured in
  `WorkspaceContext::RebindProjectState`.
- `DebugService debug_service_` on the shell; thin forwarders live in the existing
  protocol-service companion `WorkspaceShellLsp.cpp` (kept there to respect the
  `WorkspaceShell*.cpp` companion-count invariant — the behavior is on the service).
- Dedicated `dap_event_type_` + `consume_dap_callbacks` drained each frame next to
  the LSP pump (`WorkspaceEventOrchestrator`, `WorkspaceShellBootstrapper`,
  `WorkspaceLifecycleCoordinator`).
- Plugin seam: `PluginHost::ContributedDebugAdapter`,
  `ParseDebugAdapterRegistration` (type defaults to the local id),
  `RegisterDebugAdapter`, `ctx.debug.add` bound via `PluginHostLuaApi.inc` /
  `PluginLuaContextInterop.*`, gated on `capabilities.process.exec` + contribution
  sandbox; teardown prunes a plugin's adapters on unload. Reconciled into the
  per-project `DapManager` in `WorkspaceShellPlugins.cpp`.
- Console: DAP `output` events stream into the `debug.console` output channel;
  `ShowDebugConsole` surfaces it on session start.
- Toggle + commands: `debug.enabled` (user scope, default off);
  `ActionId::StartDebugging`/`StopDebugging` (`debug-start`/`debug-stop`), gated on
  the toggle.

Tests added:

- `tests/DebugServiceTests.cpp` — real Python mock-adapter lifecycle with and
  without `configurationDone`, unknown-adapter-type rejection, reconcile drop (4).
- `tests/PluginHostTests.cpp` — `ctx.debug.add` registration (explicit + defaulted
  type) and the `capabilities.process.exec` gate (2).

## Phase 2 — what shipped (2026-06-17)

Files added:

- `src/editor/BreakpointStore.{h,cpp}` — per-project, adapter-agnostic store keyed
  by file path (reuses `DiagnosticsStore`-style path normalization). Lines are
  0-based buffer indices; `enabled` + reserved `condition`/`hit_condition`/
  `log_message` persist, while `verified`/`adapter_id`/`verify_message` are
  transient. `Toggle`/`Set`/`Remove`, `FindByPath` (sorted), `SnapshotAll`,
  `ApplyVerification` (index-primary, line-fallback), `ReplaceAll`, `revision()`.
  Lives on `ProjectWorkspaceState` next to `diagnostics_store`.
- `src/editor/BreakpointRender.{h,cpp}` — gutter-dot geometry + draw (filled disc
  via horizontal spans; solid `theme.breakpoint` when verified, dimmed
  `theme.breakpoint_unverified` otherwise). Distinct from the diagnostic bar
  (`gutter_x + 2`) and the right-edge fold marker.
- `src/workspace/WorkspacePersistenceBinaryFormatDebug.cpp` — `Encode/
  DecodeDebugStateRecord` for `PersistedDebugState` (breakpoints + launch configs
  + selected index), schema-tag/version gated like the session record.
- `tests/BreakpointStoreTests.cpp` (7), `tests/fuzz/DebugStateRecordFuzz.cpp`.

Key decisions (locked):

- **`setBreakpoints` fires in stream order, not awaited.** On the `initialized`
  event `DebugSession` sends one `setBreakpoints` per file, then
  `configurationDone`. The single ordered `DapClient` stream guarantees the
  adapter receives them in order (the DAP handshake requires send-order, not
  response-order); verification reflects back asynchronously via
  `on_breakpoints_verified`. Confirmed against real debugpy (see validation).
- **Rendering via the view model (R1).** `EditorViewModel.breakpoint_gutter_marks`
  is populated by `RenderViewModelBuilder::BuildEditorViewModelInto` (gated on
  `debug.enabled` + a `BreakpointStore*`), consumed by `EditorViewRenderer`'s
  gutter loop with a cursor walk mirroring `fold_gutter_marks`. The pure render TU
  reads no project state and materializes no strings.
- **Separate `debug` persistence file**, `project_state_directory()/"debug"`,
  capability_flags = 5, saved/restored inside the existing `SaveSessionState`/
  `RestoreSessionState` hooks (restore runs before any early return so it survives
  empty-tab projects). Launch config `arguments` persist as a serialized JSON
  string; a corrupt string falls back to Null without nuking the record.
- **Session↔store coupling is callback-only.** `DebugSession::Callbacks` gained a
  `breakpoint_provider` (pulled at `initialized` and on live re-send) and
  `on_breakpoints_verified` (pushed per file). `DebugService` wires both to the
  project's `BreakpointStore` and an editor-redraw op; `StopDebugging` resets
  verification. No friend classes; the shell stays a thin forwarder
  (`ResendBreakpointsForFile`).

Wiring:

- Gutter click toggle in `WorkspaceEditorMouseCoordinator` occupies the gutter
  left of the fold hit zone (`[editor_rect.x, gutter_right - 18)`), gated on
  `debug.enabled`; toggling routes `on_breakpoint_toggled` → shell
  `ResendBreakpointsForFile` so an active session re-sends that file live.
- `DapProtocol::MakeSetBreakpointsArguments` builds `{source:{path},breakpoints:
  [{line(+1),condition?,hitCondition?,logMessage?}]}`; Phase 6 fields are gated on
  the matching capabilities.
- **`ctx.debug.addConfig` plugin seam** mirrors `ctx.debug.add`:
  `PluginHost::ContributedLaunchConfig` + `ParseLaunchConfigRegistration` +
  `RegisterLaunchConfig` + `LuaDebugAddConfig` on the `ctx.debug` module
  (`add`/`addConfig`), gated on `capabilities.process.exec`, pruned on unload.
  Reconciled into `ProjectWorkspaceState.launch_configs` in `WorkspaceShellPlugins`
  (live contributed set authoritative; persisted set is the pre-reload fallback;
  the user's selected index is preserved/clamped).
- `StartDebuggingWithDefaultConfig` launches the selected config when its adapter
  type is registered, else falls back to the first registered adapter (Phase 1
  behavior). No config-picker UI yet (deferred).

Tests added: `BreakpointStoreTests` (7), `PersistedStateRecord/DebugState*` (2),
`DebugService/SessionSendsBreakpointsBeforeConfigurationDone` (real-subprocess
mock adapter asserting 1-based lines, verification, and send-order), `DapProtocol`
setBreakpoints shape, `PluginHost/LaunchConfig*` (2), `DebugStateRecordFuzz`.

## Phase 3 — what shipped (2026-06-17)

Files added:

- `src/workspace/DebugViewModel.h` — host-owned, transient `DebugExecutionView`
  (`stopped`, `thread_id`, `stop_reason`, `frames`, `focused_frame_index`) +
  `DebugStackFrameView` (frame id, normalized `source_path`, 0-based `line`, and
  **prebuilt** `display_primary`/`display_secondary` so render TUs never build
  strings). Lives on `ProjectWorkspaceState.debug_execution`; never persisted —
  rebuilt on every `stopped`, cleared on resume/stop.
- `src/editor/ExecutionLineRender.{h,cpp}` — gutter execution-arrow geometry +
  draw (a right-pointing triangle in `theme.debug_execution_arrow`, footprint
  matched to the breakpoint dot so it overlays cleanly when stopped on one).

Key decisions (locked):

- **`stopped` → `stackTrace` directly (no `threads` first).** The stopped event
  carries the `threadId`; `DebugSession` requests `stackTrace{threadId,startFrame:0,
  levels:0}` and fires `Callbacks::on_stopped(stop, frames)` on success. A separate
  `threads` round-trip is only spent where it is actually needed — `Pause()`, which
  has no stopped thread to target (it resolves the first thread, then `pause`). The
  full thread list / selector is deferred to Phase 7. (Priority: speed.)
- **Optimistic resume.** `Continue/StepOver(next)/StepIn/StepOut` send their DAP
  command then immediately fire `on_resumed` + set `Running` so the highlight +
  Call Stack clear without waiting for a `continued` event (many adapters do not
  send one); the next `stopped` repopulates. `continued` events are still honored.
- **Execution line via the view model (R1).** `EditorViewModel.execution_line_index`
  (a `std::optional<std::size_t>`) is set by `RenderViewModelBuilder` only when
  `debug.enabled` and the focused frame's file lexically matches the viewport's
  path; `EditorViewRenderer` paints a full-width `theme.debug_execution_line` fill
  (outranking the selected-row highlight) + the gutter arrow. The pure render TU
  reads no project state and materializes no strings.
- **Call Stack panel is a peer bottom-panel tab.** `PanelContentKind::Debug` +
  `BottomPanelTabKind::Debug` add a "Call Stack" tab gated on `panel.debug.open`
  (set on the first stop, cleared on stop / tab close via the shared
  `CloseDebugPanel` helper) so the tab persists across steps even while
  `debug_execution` is momentarily empty — without coupling `TabStripService` to
  the session. Rows render the prebuilt frame strings, highlight the focused frame,
  and navigate on click (sets `focused_frame_index`, which also moves the
  execution-line highlight to the picked frame).

Wiring:

- `DebugSession`: `on_stopped`/`on_resumed` callbacks, `stopped_thread_id_`,
  `RequestStackTrace`, and `Continue/StepOver/StepIn/StepOut/Pause`.
- `DebugService`: builds `DebugExecutionView` from frames (prebuilds display
  strings, normalizes paths, DAP-line→0-based), and two new `Operations`
  (`focus_source_location`, `show_call_stack_panel`); `StopDebugging` clears the
  view. Shell wires the ops in `WorkspaceShellPlugins.cpp`; `WorkspaceShell::
  StopDebugging` calls `CloseDebugPanel`.
- Commands: `ActionId::Debug{Continue,StepOver,StepIn,StepOut,Pause}`, command-
  registry rows, availability gating (`debug.enabled` + active + Stopped for
  continue/step, Running for pause via a new `debug_session_stopped` operation),
  global-executor dispatch, and context/shell forwarders. **Default keys**
  (`WorkspaceKeybindingRegistry`): Continue=F5, Step Over=F10, Step In=F11, Step
  Out=Shift+F11. Pause has no default (F6 is taken by file-finder); bind via the
  palette. `restart` is deferred to Phase 7.
- Theme: `debug_execution_line` + `debug_execution_arrow` (with `debug-execution-
  line` / `debug-execution-arrow` theme-file keys).

Tests added: `DebugService/SessionResolvesStackOnStopAndStepsResume` (real mock
adapter: stop → stackTrace resolves the focused frames, then `next`/`stepIn`/
`stepOut`/`continue` map to their DAP commands and fire `on_resumed`),
`DebugService/SessionPauseFromRunning` (pause resolves a thread via `threads` then
sends `pause`), `RenderViewModelBuilder/MarksExecutionLineOnlyForMatchingFile`
(execution-line set only when `debug.enabled` + path matches).

## How to validate

```bash
cmake --build build -j8
./build/microide/microide_tests Dap DebugService    # focused: 19 DAP/session tests
./build/microide/microide_tests Breakpoint          # BreakpointStore (7)
./build/microide/microide_tests "PersistedStateRecord" "PluginHost/LaunchConfig"
tools/run-checks.sh tests                            # full unit suite incl. arch invariants
                                                     # (reads /tmp/microide-tests.log)
```

Fuzz the debug-state decoder:

```bash
cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/microide-fuzz --target DebugStateRecordFuzz -j8
./build/microide-fuzz/microide/DebugStateRecordFuzz -max_total_time=30 tests/fuzz/corpora/DebugStateRecordFuzz
```

Real-adapter dogfood (debugpy): the launch sequence microide uses
(initialize → launch → `initialized` → `setBreakpoints` 1-based →
`configurationDone`) was confirmed against debugpy 1.8.21 — the breakpoint binds
(`verified: true`) and a `stopped` event arrives at the line. The full C++
client/session path is exercised by the real-subprocess mock-adapter integration
test (`DebugServiceTests`).

Tracing: set `MICROIDE_TRACE_DAP_LIFECYCLE=1` for adapter lifecycle logs (mirrors
`MICROIDE_TRACE_LSP_LIFECYCLE`).

Note: `microide_perf_tests` budgets assume an optimized build; an unoptimized
`build/` dir (empty `CMAKE_BUILD_TYPE`) or a busy CPU will overshoot them. That is
unrelated to the debugger work.

## Next steps

Start **Phase 4** (Variables / Scopes panel + `setVariable`). Phase 3 already
focuses a frame (`debug_execution.focused_frame_index`, with the frame's DAP `id`
on `DebugStackFrameView`), which is the anchor the variables tree hangs off:

- New `src/workspace/DebugVariablesModel.{h,cpp}`: a lazy tree keyed by
  `variablesReference`, cleared on each stop (hook into the existing `on_stopped`
  rebuild + `on_resumed`/`StopDebugging` clears). On frame focus → `scopes`; on
  expand → `variables` (paged via `start`/`count`).
- Surface it in the Call Stack panel area (a second structured section or a peer
  "Variables" tab next to "Call Stack"); prebuild row text in the view model like
  the call-stack rows. Inline edit via `SingleLineEditor` → `setVariable` (gated on
  `capabilities.supports_set_variable`).
- DAP surface: `scopes`, `variables`, `setVariable`.

Opportunistic cleanup carried forward (per the dedup / tech-debt / UI-UX goals):

- The breakpoint / diagnostic / fold / execution gutter markers now share the same
  per-row cursor-walk shape in `EditorViewRenderer`; the execution arrow is a
  single `std::optional` (no cursor walk). If a fifth marker appears, factor a
  shared gutter-marker dispatch instead of another bespoke block.
- A config-picker UI (command palette over `launch_configs`) is the natural home
  for the persisted selection added in Phase 2.
- `Restart` (deferred from Phase 3) lands in Phase 7 with the terminate+relaunch
  fallback (`!supportsRestartRequest`).

All debugger work lands on the canonical `feat/dap` branch; do not merge to
`main` until the effort is complete.
