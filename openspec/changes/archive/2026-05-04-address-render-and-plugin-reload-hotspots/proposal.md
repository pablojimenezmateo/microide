## Why

A perf trace on `feat/tech-debt-4` (2026-04-30, default `build/` tree — no sanitizers) shows the workspace is now **noticeably slower than before the 2026-04-29 `comprehensive-tech-debt-cleanup`**. Several regressions are visible at every interaction tier — startup, project switch, idle steady-state — and at least one of them (the syntax-cache invalidation on plugin reload) is the same class of cost the prior pass was supposed to remove. The user also reports that the session ended in a plugin-related crash. Correctness, then speed, then low CPU is the priority order: a 690 ms startup tab restore and a 142 ms project switch with two back-to-back plugin reloads no longer meet that bar.

Concrete numbers from the trace (all in non-sanitizer build):

- `WorkspaceShell::RestoreSessionState::RebuildTabs`: **691 ms** on cold start (single dominating cost on the boot path).
- `ProjectCatalogCoordinator::Switch`: **142 ms** total, with **two** `ReloadPluginsForCurrentProject` runs in one switch (~12 ms + ~124 ms; the second includes a **107 ms** `InvalidateSyntaxCaches`).
- `WorkspaceShell::RenderCompareSurface`: **7–13 ms** on partial-render frames where no compare/merge tab is active.
- `Application::Render(partial)` for a single 1-dirty-rect 1-clip frame: consistently **~5 ms**, of which ~3–4 ms is `WorkspaceRootView::Render` doing whole-workspace work that should be scoped.
- `Application::PresentRetainedScene` first-frame: **12.8 ms** (then 1–1.5 ms steady, with occasional 5 ms spikes).
- Plugin runtime: probable lifetime/ordering bug at shutdown — user observed a crash; perf trace shows the host being shut down and re-initialised aggressively during project switch.

Plugin support is an active expansion phase (per `CLAUDE.md`), so a plugin-runtime crash and a syntax-cache stampede on every plugin reload directly threaten the next phase of work. This change recovers the lost speed and hardens the plugin-reload path before more capabilities land on top of it.

## What Changes

- **Gate `RenderCompareSurface` on an active compare tab** so partial-frame redraws don't pay 7–13 ms when no compare/merge surface is visible. The early-out already exists at the top-level `RenderActiveWorkspaceSurface` call site (`src/workspace/WorkspaceShellCompareRender.cpp:173`), but the per-frame partial-clip loop is reaching the helper through another path that bypasses it; close that hole.
- **Eliminate the double plugin reload on project switch.** `ProjectCatalogService::ActivateProjectState` (`src/workspace/ProjectCatalogService.cpp:70-104`) calls `initialize_current_project` (which already reloads plugins) and then unconditionally calls `reload_plugins_for_current_project(true, false, false)` again at line 101. Collapse into a single reload per activation, with the `reload_syntax_definitions` flag decided once based on whether the activation actually crossed a project root.
- **Stop the syntax-cache stampede after every project switch.** `WorkspaceShellPlugins.cpp:315-322` invalidates all syntax caches whenever `reload_syntax_definitions && plugin_runtime_.syntax_definitions_changed()`. Today the activation path always passes `true`; restrict it to the case where the active plugin set actually contributes new/changed syntax definitions, and cap invalidation to tabs that consume the changed languages.
- **Make `RestoreSessionState::RebuildTabs` lazy.** Today the loop in `src/workspace/WorkspacePersistenceCoordinatorSession.cpp:234-474` materialises every persisted tab eagerly: opens buffers, applies editor preferences, rebuilds split trees. Restore only the active tab + recently-active tabs eagerly; defer the rest to a "first-display" hydration step. Target ≤ 100 ms `RebuildTabs` for a typical session (was 691 ms).
- **Tighten the partial-clip render path.** `WorkspaceShell::PrepareRenderFrame` is being called with full-workspace work (mouse-state sync, layout recompute, split-tree normalize) even on 1-dirty-rect frames. Move per-frame work that doesn't depend on the dirty region into `Application::WorkspacePrepareFrame` once per frame instead of per partial-clip iteration; keep only clip-bounded work inside the per-clip loop.
- **Investigate and fix the plugin-host shutdown/reload lifetime issue.** `src/plugin/PluginHostPublicApi.inc` cancels async callbacks then tears down plugins, but there is no explicit join against the host's background thread pool, and `async_process_state` is held by `shared_ptr` with callbacks captured by raw plugin pointer. During a rapid init→activate→reload sequence (which the perf trace shows happening during project switch), a cancelled-but-still-queued Lua callback can outlive its plugin instance. Add a deterministic drain/join in `LuaRuntime::Shutdown` and `Reload`, and a regression test.
- Add a perf-harness scenario for "open project → switch project → idle 30 frames" so this regression is detectable from CI going forward.

## Capabilities

### New Capabilities
*(none — this is a regression-recovery pass against existing capabilities)*

### Modified Capabilities
- `performance-budgets`: tighten budgets that the trace shows are currently being exceeded (`RebuildTabs`, project switch, idle partial-frame cost, `RenderCompareSurface` gating) and add an explicit "post-switch syntax-invalidation must be scoped" requirement.
- `workspace-architecture`: codify the single-reload-per-activation rule and the `RenderCompareSurface` activation gate as do-not-regress invariants alongside the existing 2026-04-29 list.

## Impact

Affected source:

- `src/workspace/WorkspacePersistenceCoordinatorSession.cpp` (lazy tab rebuild)
- `src/workspace/ProjectCatalogService.cpp` (single reload per activation)
- `src/workspace/WorkspaceProjectStateCoordinator.cpp` (`ReloadPluginsForCurrentProject` flag plumbing)
- `src/workspace/WorkspaceShellPlugins.cpp` (scoped syntax-cache invalidation)
- `src/workspace/WorkspaceShellCompareRender.cpp` (close the partial-frame bypass on the compare gate)
- `src/workspace/WorkspaceShell.cpp` / `RenderViewModelBuilder` (move full-workspace prep out of the partial-clip loop)
- `src/plugin/PluginHostPublicApi.inc`, `src/plugin/PluginHost.cpp`, `src/plugin/LuaRuntime.*` (deterministic shutdown/reload drain)
- `tests/`: new regression coverage for the gating, the single-reload contract, and the plugin shutdown drain
- `tests/perf-harness/`: new "switch + idle" scenario; update budgets in `openspec/specs/performance-budgets/spec.md`

Build / runtime: no public ABI changes, no new dependencies. Plugin host shutdown becomes synchronous-with-drain — plugins that depended on fire-and-forget callbacks surviving past `Shutdown()` will need to migrate, but per `guidelines/plugins.md` that was never a supported contract.

Measurement note: all perf comparisons use the default `build/` tree (and `microide-perf` preset for harness runs). Sanitizer presets (`microide-asan`, `-ubsan`, `-tsan`) carry their own overhead and are not used for budget validation — only for correctness checks on the plugin-runtime drain fix.
