# Debugger / DAP Integration

Status: **active, dedicated phase** (promoted from the durable non-goal list on
2026-06-17). Phases 0–6 are done on `feat/dap`; Phase 7 (polish) is next. This
document is the single self-sufficient source of truth for the debugger effort — a
fresh agent on any machine should be able to read this file and continue without
external context.

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

### Phase 4 — Variables / Scopes panel + `setVariable` ✅ DONE (2026-06-17)

See "Phase 4 — what shipped" below. Summary: a peer **"Variables"** bottom-panel
tab (next to "Call Stack") renders the focused frame's scopes/locals as a lazily
expanded tree (`DebugVariablesModel`); scopes are fetched eagerly on each stop and
the first scope auto-expands, child `variables` are fetched lazily on expand, and a
leaf value can be edited inline (`SingleLineEditor` → `setVariable`, gated on
`supportsSetVariable`). Switching call-stack frames re-fetches the tree.

Original plan, retained for reference:

- New: `src/workspace/DebugVariablesModel.{h,cpp}` (lazy tree keyed by
  `variablesReference`, cleared on each stop).
- On frame focus → `scopes`; on expand → `variables` (paged via `start`/`count`);
  inline edit via `SingleLineEditor` → `setVariable`. Row text prebuilt in the view
  model. DAP surface: `scopes`, `variables`, `setVariable`.

### Phase 5 — Hover-to-inspect via `evaluate` ✅ DONE (2026-06-17)

See "Phase 5 — what shipped" below. Summary: while a session is `Stopped` and
`debug.enabled` is on, hovering an identifier in the **focused frame's source file**
resolves it as an expression and shows its `evaluate(context:"hover")` value in a
hover popup. The editor hover pipeline is synchronous but `evaluate` is async, so the
result is cached in a transient, frame-scoped `DebugHoverModel` (keyed by `(frame id,
expression)`, generation-guarded): a cache *hit* is served by the const hover
resolver, a *miss* kicks off the async request which on completion requests an editor
redraw that re-resolves into a hit. Gated on `supportsEvaluateForHovers`.

Original plan, retained for reference:

- Add `EditorHoverTarget::Kind::DebugValue` in `WorkspaceShellHoverTargets.cpp`;
  when paused + hovering a token, resolve its range and
  `evaluate(expr, frameId, context:"hover")`, caching like plugin hover; render via
  `WorkspaceShellHoverPopup.cpp`. DAP surface: `evaluate` (`context:"hover"`).

### Phase 6 — Conditional / hit-count / logpoints + watch expressions ✅ DONE (2026-06-17)

See "Phase 6 — what shipped" below. Summary: a right-click breakpoint-gutter
context menu edits condition/hit-count/log-message via the shared prompt surface
(re-sending `setBreakpoints` live); a new persisted **Watch** panel (third peer
tab) re-evaluates each expression with `evaluate(context:"watch")` on every stop
and frame switch. The Variables + Watch trees now share an extracted
`DebugValueTree` core (dedup).

Original plan, retained for reference:

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

## Phase 4 — what shipped (2026-06-17)

Files added:

- `src/workspace/DebugVariablesModel.{h,cpp}` — the lazy Variables tree for the
  focused frame. Source of truth is a node tree keyed by a stable monotonic
  `node_id` (with a `variablesReference → node` index); a prebuilt **flat row
  list** (`DebugVariableRowView`: prebuilt `display_name`/`display_value`/
  `display_type` + `depth`/`has_children`/`expanded`/`editable`/`node_id`) is
  rematerialized on every change so render/click are O(1) by row index (mirrors
  `debug_execution.frames`). Owns the inline-edit state (`SingleLineEditor` buffer +
  `editing_node_` + `selected_row_`). Lives on `ProjectWorkspaceState.debug_variables`,
  transient — cleared on resume/stop, never persisted. Performs **no I/O**;
  `DebugService` drives the requests and feeds responses back through `Apply*`.

Key decisions (locked):

- **Peer "Variables" tab, not a split panel.** A second `BottomPanelTabKind::
  DebugVariables` / `PanelContentKind::DebugVariables` tab sits next to "Call Stack",
  reusing the whole tab-strip/scroll/click infrastructure verbatim; both tabs share
  `panel.debug.open` so they appear/disappear together. A new `IsDebugPanelContent()`
  helper folds the Debug-family special-case (was forking across `CloseDebugPanel`,
  `BottomPanelShowsDebug`, render) into one predicate.
- **Eager scopes, lazy variables.** `DebugService::FocusFrame` (called on each stop
  for the top frame and on a call-stack frame switch) issues one `scopes` request
  and **auto-expands the first scope** (conventionally "Locals") for immediate
  visibility; structured children are fetched lazily on expand (`ToggleRow` returns
  the `variablesReference` to fetch, else 0). This matches the speed → low-CPU
  priority: one cheap request per stop, the expensive fetches only on demand.
- **Authoritative `setVariable`.** Inline edit commits via `session->SetVariable`
  (gated on `supportsSetVariable` at the session layer; the UI also gates edit
  entry). The row's value updates only from the adapter's **returned** (possibly
  normalized) value — never the raw typed text. Edit mode exits immediately on
  commit; the value lands when the response arrives.
- **Variables value field renders its own static caret.** A new
  `TextInputSurface::DebugVariableEdit` routes typing to `debug_variables.EditBuffer()`,
  but the field draws its own non-blinking caret in the bottom-panel TU (like the
  Settings field) so it stays out of the shared caret-blink machinery — no idle
  wake-ups (low-CPU priority).
- **`setVariable`/`scopes`/`variables` request encoding lives in `DapProtocol`** as
  `Make*Arguments` (+ `ParseSetVariableResult`); the Phase 3 `stackTrace` arg-building
  was moved out of `DebugSession` into `MakeStackTraceArguments` so all DAP request
  encoding now lives in the one protocol TU (its own stated invariant).

Wiring:

- `DebugSession`: `RequestScopes`/`RequestVariables`/`SetVariable` (inline callbacks,
  not lifecycle events — the same shape as `Pause()`'s `threads` round-trip).
- `DebugService`: `FocusFrame`/`ToggleVariableRow`/`BeginVariableEdit`/
  `CommitVariableEdit`/`CancelVariableEdit`; clears `debug_variables` on resume/stop;
  `on_stopped` calls `FocusFrame` for the top frame.
- Input: panel mouse (`WorkspacePanelMouseCoordinator`) — single-click a parent row
  expands/collapses, double-click a leaf value begins edit, a call-stack frame click
  fires `on_debug_frame_focus_changed` → `FocusFrame`; keyboard
  (`HandleDebugVariablesKeyDown`) — Up/Down select, Left/Right collapse/expand,
  Enter expand-or-edit, Enter/Escape commit/cancel while editing. `bottom_panel_line_count`
  now reports the Debug/Variables row counts so wheel + scrollbar work in those panels.
- The on-stop "show panel" op no longer yanks the user off the Variables tab back to
  Call Stack on every step (only switches when no debug panel is currently shown).

Tests added (`tests/DebugServiceTests.cpp`, `tests/DapProtocolTests.cpp`):

- `DapProtocol/EncodesVariablesRequests` — `MakeStackTrace/Scopes/Variables/
  SetVariableArguments` (paging omitted when zero) + `ParseSetVariableResult`.
- `DebugService/SessionVariablesTreeAndSetVariable` — real mock-adapter (`variables`
  mode): stop → scopes → expand Locals → x/obj rows → expand obj → nested depth-2
  field → collapse → `setVariable` echoes the new value into the row.
- `DebugService/SessionSetVariableGatedOnCapability` — an adapter without
  `supportsSetVariable` rejects `setVariable` (callback `ok=false`, nothing on the wire).
- `DebugService/VariablesModelTreeBehavior` — pure `DebugVariablesModel`: flatten
  ordering/depth, lazy expand/collapse + no-refetch, `setVariable` application,
  edit-target resolution, selection clamping, `Clear`.

## Phase 5 — what shipped (2026-06-17)

Files added:

- `src/workspace/DebugViewModel.h` gained `DebugHoverModel` — the transient
  hover-to-inspect cache for the focused frame's most recent
  `evaluate(context:"hover")`. Keyed by `(frame_id, expression)`; a monotonic
  `generation` (bumped on `Begin`/`Clear`) lets a completion that lands after a frame
  switch / resume be dropped. `Classify` returns `Miss/Pending/Hit/Failed`; `Begin`
  marks Pending + returns the generation; `Resolve`/`Fail` are generation-guarded.
  Lives on `ProjectWorkspaceState.debug_hover` next to `debug_execution`/
  `debug_variables`; never persisted — cleared on resume/stop and on a frame switch.

Files changed:

- `src/editor/TextLayout.{h,cpp}` — `IdentifierRangeAt(line, text_column)` returns the
  `[A-Za-z0-9_]+` `ByteRange` under a byte column (empty off an identifier). The bare
  word is the hover expression; member-access (`a.b`/`a->b`) is a narrow TODO, not done.

Key decisions (locked):

- **Async cache served by a synchronous resolver (R1).** The hover pipeline is
  synchronous (`UpdateEditorHover` → `EditorHoverTargetAtPosition`, both effectively
  read-only), but `evaluate` is async. The const resolver
  `DebugValueHoverTargetForViewport` serves a cache *hit*, returns nothing for
  *pending*/*failed* (no placeholder popup → no flicker), and on a *miss* records a
  `mutable PendingHoverEval` slot. The non-const `UpdateEditorHover` reads that slot
  and calls `DebugService::EvaluateHover`, whose completion `request_editor_redraw`s →
  the next frame's resolution re-classifies as a hit. No DAP I/O in the const path; no
  `const_cast`. The per-`(frame,expr)` dedup in `EvaluateHover` keeps the chatty
  per-frame hover trigger to one request per token.
- **Render-surface state flows through the view model.** `WorkspaceShellHoverTargets.cpp`
  is a lint-covered render-surface TU and must not read `context_.current_project_state`
  directly. `HoverTargetsViewModel` gained `debug_execution` + `debug_hover` pointers,
  populated by `BuildHoverTargets(debug_hover_enabled)` only when the shell-computed gate
  holds (`DebugEnabled()` + `IsDebugSessionStopped()` + `SupportsEvaluateForHovers()`).
- **Focused-frame-file gate (correctness).** Hover evaluates only over the focused
  frame's source file (lexically-normalized path match, same as the execution-line
  marker), since `evaluate(frameId)` resolves names in that frame's lexical scope.
- **Shared two-block hover-card helper (dedup).** `ComputeTwoBlockHoverCardRect` +
  `DrawTwoBlockHoverCard` in `WorkspaceShellHoverPopup.cpp` capture the
  "muted title line(s) + gap + primary content line(s)" shape; the Plugin (title/content)
  and DebugValue (type/value) popups both route through them instead of a fourth
  near-duplicate branch. Blame/Diagnostic are unchanged.

Wiring:

- `DapProtocol::MakeEvaluateArguments(expression, frameId, context)` (omits `frameId`
  when 0) joins the other `Make*Arguments`; `ParseEvaluateResult` already existed.
- `DebugSession::RequestEvaluate(expression, frameId, context, cb(ok, result))` mirrors
  `RequestScopes`/`SetVariable`; hover context is gated on `supportsEvaluateForHovers`
  (callback fires `ok=false` when unsupported, nothing on the wire).
- `DebugService::EvaluateHover(frameId, expression)` (dedup → `Begin` → `RequestEvaluate`
  → generation-guarded `Resolve`/`Fail` → `request_editor_redraw`) +
  `SupportsEvaluateForHovers()`. `debug_hover.Clear()` runs alongside the existing
  `debug_execution`/`debug_variables` clears on stop/resume **and in `FocusFrame`**.
- `WorkspaceShell::DebugEnabled()` centralizes the `debug.enabled` bool parse for shell
  code. `EditorHoverTarget::Kind::DebugValue` + `DebugHoverValue{value,type}` probe
  between Diagnostic and Plugin (debug values outrank LSP hover while paused).

Tests added (`tests/DebugServiceTests.cpp`, `tests/DapProtocolTests.cpp`,
`tests/TextRendererTests.cpp`):

- `DapProtocol/EncodesVariablesRequests` extended — `MakeEvaluateArguments` shape +
  `frameId` omitted when 0.
- `TextLayout identifier range at cursor` — `IdentifierRangeAt` mid-word/boundaries/
  whitespace/punctuation/tabs + no member-access merge.
- `DebugService/SessionEvaluateHover` — real mock-adapter (`evaluate` mode): stop →
  `Begin`/Pending → `RequestEvaluate` → `Resolve` → Hit with the adapter's value/type.
- `DebugService/SessionEvaluateGatedOnCapability` — adapter without
  `supportsEvaluateForHovers` rejects the hover evaluate (callback `ok=false`).
- `DebugService/HoverModelBehavior` — pure `DebugHoverModel`: key classification,
  generation guard dropping a stale completion, `Clear`.

## Phase 6 — what shipped (2026-06-17)

Files added:

- `src/workspace/DebugValueTree.{h,cpp}` — the shared lazy value-tree core
  extracted from `DebugVariablesModel` (node store keyed by stable id +
  `variablesReference` index, flatten → `DebugVariableRowView` rows, lazy
  expand/`ApplyVariables`, `ApplySetVariable`/`SetNodeValue` via a shared
  `RebindReference`, selection cursor, inline `SingleLineEditor` edit + commit
  target). `DebugVariablesModel` is now a thin wrapper owning one (adding only
  `BeginFrame`/`ApplyScopes`); its public API and the `VariablesModelTreeBehavior`
  test are unchanged, guarding the refactor.
- `src/workspace/DebugWatchModel.{h,cpp}` — the Watch panel model: a persistent
  ordered expression list + a transient `DebugValueTree`. `BeginEvaluation`
  pre-creates one placeholder root per expression (stable, ordered rows) and
  `ApplyEvaluate(index, …)` folds each async result in by index; structured
  results expand via the shared tree. `Add/Edit/RemoveExpression`,
  `SetExpressions` (restore), `ExpressionIndexForRow` (root vs child),
  `ClearResults` (keeps expressions, blanks values).

Key decisions (locked):

- **Breakpoint editing reuses the tree context menu + the prompt surface (dedup),
  not a bespoke overlay.** A gutter right-click opens the existing
  `TreeContextMenuState` machinery (a new `TreeContextTargetKind::BreakpointLine`
  carrying a `line`); items route through four context-menu-only `ActionId`s
  (`DebugBreakpointEdit{Condition,HitCondition,LogMessage}` / `…Remove`, no
  command-registry specs so they stay out of the palette). Editing opens a
  single-line `PromptSurfaceService` prompt (`SetBreakpoint{Condition,HitCondition,
  LogMessage}` actions + a `target_line` field); commit writes the
  `BreakpointStore` modifier (empty clears) and live-re-sends `setBreakpoints`.
  Because the right-click is preempted by the editor context menu in
  `WorkspaceShellMouse`, a dedicated side-effect-free
  `EditorMouseCoordinator::HandleGutterContextMenu` is dispatched ahead of it.
- **Capability gating stays at the wire (correctness).** Editing a modifier is
  always allowed (the field persists); `DebugSession::SendBreakpointsForFile`
  already drops condition/hit/log when the adapter lacks the matching capability,
  so a stored-but-unsupported modifier is simply not sent — no menu-level gating
  needed.
- **Watch is a third peer tab mirroring Phase 4.** `PanelContentKind::DebugWatch`
  + `BottomPanelTabKind::DebugWatch` add a "Watch" tab under the same
  `panel.debug.open` gate and `IsDebugPanelContent` family; the bottom-panel
  render shares a generic `draw_value_tree_row` lambda between Variables and Watch.
  Mouse/keyboard mirror the Variables coordinators (single-click expand,
  double-click/Enter edit, Insert add, Delete remove, prompt-based expression
  add/edit). An empty-state hint makes the first add discoverable.
- **Watch re-evaluation is driven from `FocusFrame` (speed/correctness).**
  `DebugService::EvaluateWatches(frame_id)` runs inside `FocusFrame`, so it fires
  on every stop (top frame) and on a call-stack frame switch, evaluating each
  expression in the focused frame's scope with `context:"watch"` (uncapped, unlike
  hover). Results clear on resume/stop (`ClearResults`, keeping the expression
  list).
- **Persistence is an additive tag with no schema bump (correctness).**
  `PersistedDebugState.watch_expressions` serializes as repeated
  `DebugStateTag::WatchExpression` string records. The tag stream skips unknown
  tags and the schema check hard-rejects version mismatches, so an additive tag is
  fully backward/forward compatible — bumping `kSchemaVersion` would have silently
  discarded existing persisted breakpoints. Saved/restored in the existing
  `Save/RestoreDebugState` hooks.

Tests added:

- `tests/BreakpointStoreTests.cpp` — `ModifierSetters` (find-or-create, clear on
  nullopt, revision bumps, no-op skip).
- `tests/DebugServiceTests.cpp` — `WatchModelBehavior` (pure model: placeholder
  roots, `ApplyEvaluate`, lazy expand, `ExpressionIndexForRow`, edit/remove,
  `SetExpressions`/`ClearResults`) and `SessionWatchEvaluate` (real mock adapter:
  stop → `evaluate(context:"watch")` folds onto a watch root).
- `tests/DapProtocolTests.cpp` — `MakeEvaluateArguments(context:"watch")` shape.
- `tests/PersistedStateRecordTests.cpp` — watch expressions round-trip +
  `DebugStateBackwardCompatNoWatch` (a watch-free record decodes with breakpoints
  intact). `DebugStateRecordFuzz` exercises the new tag (no target change).

## How to validate

```bash
cmake --build build -j8
./build/microide/microide_tests Dap DebugService    # focused: 28 DAP/session tests
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
test (`DebugServiceTests`). For Phase 5, enable `debug.enabled`, stop at a
breakpoint, and hover a local in the stopped file — the value popup appears (and
updates when you switch call-stack frames). The mock adapter now has an `evaluate`
mode advertising `supportsEvaluateForHovers`.

Tracing: set `MICROIDE_TRACE_DAP_LIFECYCLE=1` for adapter lifecycle logs (mirrors
`MICROIDE_TRACE_LSP_LIFECYCLE`).

Note: `microide_perf_tests` budgets assume an optimized build; an unoptimized
`build/` dir (empty `CMAKE_BUILD_TYPE`) or a busy CPU will overshoot them. That is
unrelated to the debugger work.

## Next steps

Start **Phase 7** (polish: multi-thread, multi-session, exception breakpoints,
restart). The pieces already in place to build on:

- **Multi-session:** `DapManager` already owns the active `DebugSession` and each
  `DapClient` owns its own `seq`/`pending_requests`, so N concurrent sessions need
  an active-session switcher + event routing by originating client, not a protocol
  change. The transient view models (`debug_execution`/`debug_variables`/
  `debug_watch`/`debug_hover`) already live per-project on `ProjectWorkspaceState`.
- **Multi-thread:** `stopped`→`stackTrace` currently targets the stopped thread
  directly (Phase 3 speed decision); a thread selector in the Call Stack panel
  needs a `threads` round-trip + a focused-thread field on `DebugExecutionView`.
- **Exception breakpoints:** `setExceptionBreakpoints(filters)` from
  `capabilities.exceptionBreakpointFilters`; surface the filters as toggles
  (natural home: the Breakpoints area / a small settings list).
- **Restart:** `restart` with a terminate+relaunch fallback when
  `!supportsRestartRequest` (deferred since Phase 3). Add `DebugRestart` alongside
  the other execution-control `ActionId`s.
- Run the **TSAN preset** once multi-session concurrent I/O lands.

Opportunistic cleanup carried forward (per the dedup / tech-debt / UI-UX goals):

- The Variables + Watch trees now share `DebugValueTree`; if a third value-tree
  surface appears (e.g. a REPL result view, or inline hover expansion), build it on
  `DebugValueTree` too rather than re-deriving the node/flatten machinery.
- The bottom-panel structured rows now share `draw_two_column_row` **and** the
  generic `draw_value_tree_row` lambda in `WorkspaceShellRenderBottomPanel.cpp`; a
  fourth structured debug panel should reuse them.
- Breakpoint condition/hit/log + watch add/edit all route through the shared
  `PromptSurfaceService` single-line prompt; a future REPL input or watch-from-
  selection action should reuse it rather than a bespoke field.
- The breakpoint / diagnostic / fold / execution gutter markers share the same
  per-row cursor-walk shape in `EditorViewRenderer`; if a fifth marker appears,
  factor a shared gutter-marker dispatch. A render distinction for
  conditional/logpoint dots (deferred from Phase 6 as optional polish) is the most
  likely next gutter change.
- The Plugin and DebugValue hover popups share `ComputeTwoBlockHoverCardRect` /
  `DrawTwoBlockHoverCard`; route the Diagnostic popup through them if its layout is
  ever made byte-identical.
- The `DebugHoverModel` async-cache shape (Begin/Classify/Resolve/Fail + generation
  guard) is a template if a hover popup later wants to **expand a structured value
  inline** — and `DebugValueTree` now models the expandable side directly.
- A config-picker UI (command palette over `launch_configs`) is the natural home
  for the persisted selection added in Phase 2.

All debugger work lands on the canonical `feat/dap` branch; do not merge to
`main` until the effort is complete.
