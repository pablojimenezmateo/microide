## ADDED Requirements

### Requirement: Project Switch Single Plugin Reload

A single `ProjectCatalogService::ActivateProjectState` call SHALL invoke the plugin runtime reload path at most once. Reactivation of an already-initialised project state SHALL NOT call `LuaRuntime::Reload` and SHALL NOT trigger a full syntax-cache invalidation; it SHALL only refresh plugin-owned UI surfaces (sidebar entries, command registry views) that depend on the active project.

#### Scenario: Reactivation does not reload the plugin runtime
- **WHEN** a project state that already has `initialized = true` is activated again (e.g. via `ProjectCatalogCoordinator::Switch` returning to a recently-active project)
- **THEN** the perf trace SHALL contain at most one `WorkspaceShell::ReloadPluginsForCurrentProject` scope for that activation, and `WorkspaceShell::ReloadPluginsForCurrentProject::PluginRuntimeReload` SHALL NOT appear

#### Scenario: First activation reloads exactly once
- **WHEN** a project state with `initialized = false` is activated for the first time in a session
- **THEN** `WorkspaceShell::ReloadPluginsForCurrentProject` SHALL appear exactly once in the activation trace, and `ProjectCatalogService::ActivateProjectState::ReloadPluginsForCurrentProject` SHALL NOT appear as a second sibling scope

### Requirement: Scoped Syntax Cache Invalidation

When a plugin reload reports changed syntax definitions, the workspace SHALL invalidate cached tokenization only for tabs whose buffer language is in the changed-language set. An empty changed-language set SHALL produce zero invalidation work.

#### Scenario: No language definitions changed across reload
- **WHEN** `LuaRuntime::Reload` completes with an empty changed-syntax-language set
- **THEN** `WorkspaceShell::ReloadPluginsForCurrentProject::InvalidateSyntaxCaches` SHALL complete in under 1 ms regardless of open tab count

#### Scenario: One language changed across reload
- **WHEN** `LuaRuntime::Reload` reports that a single language definition (e.g. `lua`) changed
- **THEN** invalidation SHALL touch only tabs whose buffer language matches that language, and the trace scope `InvalidateSyntaxCaches` SHALL scale with that subset rather than total tab count

### Requirement: Compare Surface Render Gating

Render code paths owned by the compare/merge surface SHALL execute only when the active workspace surface is a compare or merge tab. Partial-clip frame iterations SHALL NOT enter the compare-surface render code when no compare or merge tab is active, regardless of which split contains the dirty rect.

#### Scenario: Idle partial frame with no compare tab open
- **WHEN** the workspace has no compare or merge tab active in any split and the user produces a one-dirty-rect partial frame elsewhere (e.g. cursor blink, scroll bar update)
- **THEN** `WorkspaceShell::RenderCompareSurface` SHALL NOT appear in the perf trace for that frame

#### Scenario: Compare tab in inactive split
- **WHEN** a compare tab exists in a split that does not contain the active editor and a partial-clip frame redraws an unrelated region
- **THEN** `WorkspaceShell::RenderCompareSurface` SHALL NOT execute for clip rects that fall outside the compare tab's bounds

### Requirement: Lazy Session Tab Hydration

`WorkspaceShell::RestoreSessionState::RebuildTabs` SHALL eagerly hydrate only the active tab, the most-recently-active tab in each split group, and any pinned tab. Other persisted tabs SHALL be recorded as deferred handles and hydrated on first activation.

#### Scenario: Cold restore of a multi-tab session
- **WHEN** cold startup restores a session containing 20+ persisted tabs across multiple splits
- **THEN** `RebuildTabs` SHALL complete in under 100 ms on the reference host, and the deferred-tab count plus the eager-tab count SHALL equal the persisted-tab count

#### Scenario: Activating a deferred tab
- **WHEN** the user activates a tab that was restored as a deferred handle
- **THEN** the same code path that opens a tab from disk SHALL hydrate the deferred handle exactly once, and the handle SHALL be dropped after hydration

### Requirement: Per-Frame Workspace Prep Runs Once Per Frame

Whole-workspace per-frame work — mouse-state synchronisation, layout recompute, split-tree normalisation, and view-model construction — SHALL run exactly once per rendered frame, regardless of whether the frame is full or partial and regardless of the number of clip rects in a partial frame.

#### Scenario: Single-clip partial frame
- **WHEN** the application renders a partial frame with one dirty rect coalesced into one clip
- **THEN** `WorkspaceShell::PrepareRenderFrame` work (or its successor `PrepareFrameOnce`) SHALL execute exactly once for that frame, and the per-clip render body SHALL NOT re-run mouse-state sync, layout recompute, split-tree normalisation, or view-model construction

#### Scenario: Multi-clip partial frame
- **WHEN** the application renders a partial frame with N coalesced clip rects
- **THEN** the per-frame prep SHALL run once, the per-clip render body SHALL run N times, and the total wall-clock cost of the prep portion SHALL NOT scale with N

### Requirement: Plugin Runtime Shutdown And Reload Drain Workers

`LuaRuntime::Shutdown` and `LuaRuntime::Reload` SHALL synchronously drain the plugin host's background workers before tearing down plugin instances. No plugin callback SHALL fire after `Shutdown` returns. A bounded deadline (default 250 ms) SHALL apply; if exceeded, the runtime SHALL log a warning and continue, but normal-load operation SHALL return well within the deadline.

#### Scenario: Shutdown during active plugin work
- **WHEN** `LuaRuntime::Shutdown` is called while one or more plugin host worker callbacks are queued or in-flight
- **THEN** `Shutdown` SHALL return only after every queued callback has either executed to completion or been observed-cancelled, and the application SHALL NOT execute any plugin callback after `Shutdown` returns

#### Scenario: Rapid project switch under load
- **WHEN** the user rapidly switches projects while plugin callbacks are in-flight (the pattern that previously crashed the application)
- **THEN** the TSAN regression test for this scenario SHALL pass, no use-after-free SHALL be reported, and the new project SHALL activate with a clean plugin host

## MODIFIED Requirements

### Requirement: Startup Budget

MicroIDE SHALL complete cold startup (binary launch to interactive shell with the last-session project restored) within a documented budget on a reference Linux host, measured through `docs/startup-tracing.md`. Changes that affect startup paths or schedule startup-triggered background work SHALL record startup trace output and startup-to-idle evidence in the change record.

The startup budget on the reference host SHALL be: `WorkspaceShell::InitializeCurrentProject` ≤ 250 ms, of which `RestoreSessionState::RebuildTabs` ≤ 100 ms. Changes that regress either bound SHALL NOT merge.

#### Scenario: Startup regression
- **WHEN** a change modifies `src/app/*` bootstrap, workspace-session restore, plugin runtime startup, syntax snapshot loading, project file-watching startup, or startup-triggered refresh scheduling
- **THEN** the change record SHALL include `MICROIDE_STARTUP_TRACE` output before and after the change plus corresponding evidence that the application settles to near-zero idle CPU after startup completes

#### Scenario: Last-session restore correctness
- **WHEN** the previous session held open editor tabs, compare tabs, merge tabs, and terminals
- **THEN** cold startup SHALL restore each of those surfaces to its recorded state — eagerly for the active tab, the most-recently-active tab in each split, terminals, and pinned tabs; lazily (as deferred handles hydrated on first activation) for the rest — and the user-visible tab strip SHALL show every restored tab regardless of hydration mode

#### Scenario: Cold-start budget on the reference host
- **WHEN** a change is benchmarked on the reference Linux host with a 20-tab persisted session
- **THEN** `WorkspaceShell::InitializeCurrentProject` SHALL complete in ≤ 250 ms median (P95 ≤ 350 ms), and `RestoreSessionState::RebuildTabs` SHALL complete in ≤ 100 ms median (P95 ≤ 150 ms)

### Requirement: Scenario-Level Budgets For Multi-Project, Multi-Tab, Linter, And Idle Workloads

The performance harness SHALL gate merges on scenario-level budgets that cover workloads previously not measured at all: project switching, tab cycling, linter-on-save (plugin diagnostics), idle wake-ups, and switch-then-idle (the regression class that motivated the 2026-04-30 perf pass). Each scenario in `tests/perf/baselines/` SHALL act as a budget for its workload.

#### Scenario: Project-switch budget
- **WHEN** the `multi_project_switch` scenario runs (open five projects, switch among them)
- **THEN** the harness SHALL fail the merge if median project-switch wall time, frame-time percentiles, or RSS growth regress beyond the per-metric tolerance against the committed baseline, and the budget SHALL be ≤ 60 ms median, ≤ 120 ms P95 for `ProjectCatalogCoordinator::Switch`

#### Scenario: Tab-cycle budget
- **WHEN** the `multi_tab_cycle` scenario runs (open twenty file tabs in one project, cycle through them)
- **THEN** the harness SHALL fail the merge if median tab-cycle wall time or frame-time percentiles regress beyond tolerance

#### Scenario: Linter-on-save budget
- **WHEN** the `linter_on_save` scenario runs (save triggers a plugin diagnostics provider)
- **THEN** the harness SHALL fail the merge if save-to-diagnostics-published wall time regresses beyond tolerance

#### Scenario: Idle wake-up budget
- **WHEN** the `idle_soak_30s` scenario runs and the bug-detection nightly long-soak runs
- **THEN** the harness SHALL fail on SDL wake-up count exceeding a documented per-second (idle_soak_30s) and per-hour (long_soak_8h) budget

#### Scenario: Switch-and-idle budget
- **WHEN** the `switch_and_idle` scenario runs (open project A with persisted tabs, switch to project B with persisted tabs, then idle 30 frames)
- **THEN** the harness SHALL fail the merge if the median `Application::Render(partial)` for a 1-dirty-rect 1-clip frame exceeds 2.5 ms, if any frame contains an unexpected `WorkspaceShell::RenderCompareSurface` scope while no compare tab is active, or if the activation trace contains more than one `WorkspaceShell::ReloadPluginsForCurrentProject` scope per switch
