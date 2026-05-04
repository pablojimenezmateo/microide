## ADDED Requirements

### Requirement: Single Plugin Reload Per Project Activation

`ProjectCatalogService::ActivateProjectState` SHALL invoke a plugin-runtime reload at most once per call. Reactivation of an already-initialised project state SHALL route plugin-surface refresh through a dedicated `RefreshPluginSurfacesForReactivation` (or equivalent) entry point that does NOT call `LuaRuntime::Reload` and does NOT enter the syntax-cache invalidation path.

#### Scenario: Architectural lint catches double-reload regression
- **WHEN** the architectural-lint test runs as part of `ctest`
- **THEN** it SHALL fail if `ProjectCatalogService::ActivateProjectState` (or any caller it dispatches into) invokes `reload_plugins_for_current_project(...)` more than once on a single code path, and SHALL identify the offending call sites in the failure message

#### Scenario: Reactivation entry point exists and is the only path
- **WHEN** an already-initialised project state is reactivated
- **THEN** the call SHALL go through `RefreshPluginSurfacesForReactivation` (or its agreed name), and direct calls to `ReloadPluginsForCurrentProject` from the reactivation branch SHALL NOT compile (enforced via interface segregation, not lint)

### Requirement: Compare Surface Render Gating Is Structural

The view model produced by `RenderViewModelBuilder` SHALL omit the compare-surface entry whenever no compare or merge tab is active in the surface being rendered. Render translation units in `src/workspace/WorkspaceShellCompareRender*.cpp` SHALL operate on the view-model entry only and SHALL NOT consult shell context to decide whether to render.

#### Scenario: Compare render TU has no runtime active-surface check
- **WHEN** the architectural-lint test inspects `src/workspace/WorkspaceShellCompareRender*.cpp` translation units
- **THEN** it SHALL fail if any of them call `ActiveTabIsCompare()`, read `context_.current_project_state`, or otherwise consult shell state to decide whether to render; the decision SHALL be encoded as the presence or absence of a view-model field

#### Scenario: View model omits compare entry when not active
- **WHEN** `RenderViewModelBuilder` builds a frame view model and no compare/merge tab is active in any visible split
- **THEN** the produced view model SHALL NOT contain a populated compare-surface entry, and downstream render TUs that iterate the view model SHALL produce zero compare-render work

### Requirement: Per-Frame Workspace Prep Has A Single Call Site

Whole-workspace per-frame work — mouse-state synchronisation, layout recompute, split-tree normalisation, view-model construction — SHALL be invoked exactly once per rendered frame from `Application::WorkspacePrepareFrame` (or the documented equivalent). Per-clip render code SHALL NOT trigger any of this work, directly or transitively.

#### Scenario: Per-clip render path does not call frame-prep
- **WHEN** the architectural-lint test inspects per-clip render entry points (the helpers reached from `Application::WorkspaceRender(partial-clip)`)
- **THEN** it SHALL fail if any of them call `WorkspaceShell::PrepareRenderFrame` or its successor, or transitively invoke layout recompute, split-tree normalisation, or view-model construction

#### Scenario: Frame-prep is called once per partial frame
- **WHEN** the application renders a partial frame with N clip rects (N ≥ 1)
- **THEN** the perf trace SHALL contain exactly one `Application::WorkspacePrepareFrame` (or successor) scope, and the per-clip render scope SHALL appear N times without re-entering frame-prep

### Requirement: Plugin Runtime Has A Deterministic Drain Seam

`LuaRuntime` SHALL expose a single drain seam (`DrainAndJoinWorkers` or equivalent) used by both `Shutdown` and `Reload`. The seam SHALL synchronously wait for all in-flight plugin-host worker callbacks to either complete or observe their cancellation flag before returning. Plugin code SHALL NOT be invoked through any path after the seam returns.

#### Scenario: Drain seam is the only teardown precondition
- **WHEN** the architectural-lint test inspects `src/plugin/LuaRuntime.{h,cpp}` and `src/plugin/PluginHost*.cpp`
- **THEN** it SHALL fail if any code path tears down plugin instances, frees the Lua VM, or destroys `async_process_state` without first calling the drain seam, and SHALL identify the offending path

#### Scenario: Drain seam respects the bounded deadline
- **WHEN** `Shutdown` or `Reload` calls the drain seam and a worker exceeds the configured deadline (default 250 ms)
- **THEN** the seam SHALL log a warning identifying the slow worker, SHALL return, and the test for the seam SHALL verify the warning is emitted exactly once per occurrence

## MODIFIED Requirements

### Requirement: Plugin Host Is Decomposed

`PluginHost` SHALL be decomposed into a runtime core plus per-surface extension modules (commands, sidebars, syntax, diagnostics, hover, providers, lifecycle). Each module SHALL own its registry and SHALL NOT exceed 800 lines. The runtime core SHALL expose a deterministic drain seam used by every shutdown and reload path.

#### Scenario: PluginHost size invariant
- **WHEN** the source tree is built
- **THEN** no single `src/plugin/*.cpp` translation unit SHALL exceed 800 lines, enforced by the architectural-lint test

#### Scenario: Lua VM lifecycle is isolated
- **WHEN** plugin runtime work creates, suspends, or destroys a Lua VM
- **THEN** that work SHALL go through one `LuaRuntime` seam owned by the runtime core; no extension-surface module SHALL hold a raw `lua_State*`

#### Scenario: Shutdown and reload use the drain seam
- **WHEN** `LuaRuntime::Shutdown` or `LuaRuntime::Reload` is invoked
- **THEN** both SHALL call the runtime core's drain seam before tearing down plugin instances, and the architectural-lint test SHALL hard-fail if either path destroys plugin state without first invoking the seam
