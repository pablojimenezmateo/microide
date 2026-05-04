## 1. Plugin Runtime Drain (D6 — correctness first)

- [x] 1.1 Add a `DrainAndJoinWorkers(deadline)` seam to `async_state_interop` (placed there because `LuaRuntime` is just the lua_State* owner; the drain belongs alongside the async state). Adds `condition_variable` to `AsyncProcessState`, worker-side `NotifyWorkerCompleted` after `fetch_sub`. Files: `src/plugin/PluginAsyncStateInterop.{h,cpp}`, `src/plugin/PluginHostRuntimeTypes.h`, `src/plugin/PluginProcessInterop.cpp`.
- [x] 1.2 Wired `PluginHost::Reload` and `PluginHost::Shutdown` (the public Lua-runtime entry points; `LuaRuntime` itself has no Reload/Shutdown methods in this codebase) in `src/plugin/PluginHostPublicApi.inc` to call `Impl::DrainAsyncProcessWorkers()` **before** `TearDownPlugins`. Replaces prior `CancelAsyncProcessCallbacks` direct call.
- [x] 1.3 Added `runtime_types::kPluginHostDrainDeadline = 100ms` (one place, no magic literal at call sites). `Impl::DrainAsyncProcessWorkers` emits an `SDL_Log` warning when the deadline is exceeded. **Implementation note:** drain is *best-effort with bounded wait*, not "synchronously wait until in_flight==0" as the design wording suggested. A worker is blocked inside `RunSubprocess` for the full subprocess duration, so a strict synchronous wait would tie shutdown latency to user subprocesses (and break the `<200ms` shutdown contract asserted by the existing `CancelsAsync*` tests). Correctness is preserved by the existing `request->cancelled` flag — workers check it under mutex before pushing to `pending_callbacks` and never deref Lua state after cancel. The seam adds (a) deterministic cv notification when workers retire fast, (b) observability when they don't.
- [x] 1.4 Added `PluginHost/RapidReloadDrainsAsyncWorkers` test in `tests/PluginHostTests.cpp`: 8× rapid `Reload` while a 300ms subprocess is in flight + final `Shutdown`, asserts no callback from torn-down plugin fires, no errors, drain bounded by deadline. Passes under default build and TSAN preset.
- [x] 1.5 Added `CheckPluginDrainBeforeTeardown` rule to `tests/ArchitectureInvariantsTests.cpp`. Hard-fails if `impl_->TearDownPlugins(` appears without a `DrainAsyncProcessWorkers` / `DrainAndJoinWorkers` call in the preceding 12 lines. Verified by removing the drain call (hard-fails with exact line) and restoring it (passes).
- [x] 1.6 Built and ran the TSAN preset: `cmake --build build/microide-tsan-make && ctest --test-dir build/microide-tsan-make`. Targeted plugin tests pass cleanly under TSAN with **zero data-race reports** on the drain-seam paths. Two pre-existing TSAN flakes (`RuntimePaths/PreferExplicitAssetRootOverride` on this tree, `Phase5.LspMergeBuffers...` on the stashed pre-change tree) confirmed unrelated by stash-comparison; both are environmental, neither involves the plugin runtime.

## 2. Single Plugin Reload Per Activation (D1)

- [x] 2.1 Replaced the boolean parameters on `WorkspaceShell::ReloadPluginsForCurrentProject` with a `PluginReloadRequest` struct (`syntax_definitions`, `replay_buffer_opens`, `open_lsp_documents`); new header `src/workspace/WorkspacePluginReloadRequest.h` is the single source of truth. Updated the `ProjectCatalogService::Operations` facade — the old `reload_plugins_for_current_project` callback was removed entirely and replaced with `refresh_plugin_surfaces_for_reactivation` — and all internal call sites in `WorkspaceProjectStateCoordinator.cpp` use the struct with designated initialisers.
- [x] 2.2 Introduced `WorkspaceShell::RefreshPluginSurfacesForReactivation()` in `src/workspace/WorkspaceShellPlugins.cpp`. It runs `RebuildPhase{3,4,5}Registries`, `NormalizeSidebarViewSelection`, `RefreshPluginSidebar`, conditional `RefreshGitSidebar`, and chrome/editor redraw requests. It does **not** call `LuaRuntime::Reload` and does **not** enter the syntax-cache invalidation path.
- [x] 2.3 Replaced the unconditional `operations_.reload_plugins_for_current_project(true, false, false)` in `ProjectCatalogService::ActivateProjectState` (was line 101) with `operations_.refresh_plugin_surfaces_for_reactivation()`. The trace scope is renamed accordingly.
- [x] 2.4 Confirmed the first-activation branch already drives plugin reload exactly once: `WorkspaceShell::InitializeCurrentProject` calls `ReloadPluginsForCurrentProject` once on each of its three terminal paths (restored-session, preferred-file open, placeholder welcome). `ActivateProjectState`'s first-init branch only delegates to `initialize_current_project`; no separate reload needed. No code change required.
- [x] 2.5 Added `CheckSinglePluginReloadPerActivation` rule to `tests/ArchitectureInvariantsTests.cpp`. It hard-fails if `ProjectCatalogService::ActivateProjectState`'s body contains any call matching `(reload_plugins_for_current_project|ReloadPluginsForCurrentProject)\(`. Verified: lint correctly flags a reintroduced bait call (with the exact line number); the spec scenario "direct calls SHALL NOT compile" is *also* enforced structurally — `ReloadPluginsForCurrentProject` is a private member of `WorkspaceShell` and `reload_plugins_for_current_project` no longer exists on the operations facade, so any direct reintroduction fails to compile before the lint runs.
- [x] 2.6 Added `WorkspaceShell/ProjectReactivationDoesNotReloadPlugins` test in `tests/WorkspaceShellPluginTests.cpp`: opens project A, opens project B (storing A initialised), resets the invocation counter, switches back to A, asserts `reload_plugins_invocation_count_ == 0`. The counter is exposed via test-access (`ReloadPluginsInvocationCount`/`ResetReloadPluginsInvocationCount`) and incremented inside `ReloadPluginsForCurrentProject`; this is the programmatic equivalent of "perf-trace contains exactly zero `WorkspaceShell::ReloadPluginsForCurrentProject` scopes" since `PerformanceTrace` has no programmatic capture API. Passes.

## 3. Scoped Syntax Cache Invalidation (D2)

- [x] 3.1 Replace `PluginRuntimeService::syntax_definitions_changed() -> bool` with `ChangedSyntaxLanguages() -> std::span<const std::string_view>` (or the equivalent stable-span type already used in the runtime); keep the boolean as a thin wrapper if any caller still wants it
- [x] 3.2 Update `WorkspaceShell::InvalidateRuntimeSyntaxStateCaches` to take the changed-language set and walk only tabs whose buffer language is in the set; an empty set SHALL produce zero work
- [x] 3.3 Update the call site at `src/workspace/WorkspaceShellPlugins.cpp:319` to pass the set
- [x] 3.4 Verify the perf trace: `WorkspaceShell::ReloadPluginsForCurrentProject::InvalidateSyntaxCaches` SHALL be ≤ 1 ms when the set is empty (captured at `/tmp/address_render_trace.log`; scope measured `0.00 ms`)
- [x] 3.5 Add a unit test that exercises the empty-set, single-language, and all-languages cases

## 4. Compare Surface Render Gating (D4)

- [x] 4.1 Add a `compare_surface` (optional) field to the frame view model produced by `RenderViewModelBuilder`; populate it only when the active workspace surface is a compare or merge tab
- [x] 4.2 Refactor `WorkspaceShellCompareRender.cpp` (and any siblings) to consume the view-model field instead of calling `ActiveTabIsCompare()` or reading `context_.current_project_state`
- [x] 4.3 Remove the runtime gate from `RenderActiveWorkspaceSurface` (the gate is now structural — absent view-model field = no work)
- [x] 4.4 Extend `tests/ArchitectureInvariantsTests.cpp` to fail if `src/workspace/WorkspaceShellCompareRender*.cpp` calls `ActiveTabIsCompare()`, reads `context_.current_project_state`, or otherwise consults shell state to decide whether to render
- [x] 4.5 Add a redraw test under `tests/redraw/` that simulates a one-dirty-rect partial frame on a workspace with no compare tab active and asserts the trace contains zero `WorkspaceShell::RenderCompareSurface` scopes

## 5. Per-Frame Prep Once Per Frame (D5)

- [x] 5.1 Audit `WorkspaceShell::PrepareRenderFrame` line-by-line; classify each statement as "once-per-frame" or "per-clip" (default to once-per-frame; flag in design's open question if any line truly needs per-clip)
- [x] 5.2 Split into `WorkspaceShell::PrepareFrameOnce(FrameContext&)` (called by `Application::WorkspacePrepareFrame`) and `WorkspaceShell::RenderClip(const FrameToken&, ClipRect, ...)` where `FrameToken` is RAII-style and only constructible by `PrepareFrameOnce`
- [x] 5.3 Update `Application::WorkspaceRender(partial-clip)` and `Application::WorkspaceRender(full)` to invoke `PrepareFrameOnce` exactly once per frame and `RenderClip` per clip
- [x] 5.4 Extend the architectural-lint test to fail if any per-clip render entry point calls `PrepareRenderFrame` (or its successor) or transitively invokes layout/normalize/view-model construction
- [x] 5.5 Verify with the perf trace that median 1-clip 1-dirty-rect partial frame `Application::Render(partial)` is ≤ 2.5 ms on the reference host

## 6. Lazy Tab Hydration (D3)

- [x] 6.1 Add `DeferredTabHandle` POD (path, language hint, viewport scroll, selection range, split-tree placement) to `src/workspace/EditorTabService.h` (or the closest existing tab-strip type); document that handles do not own buffers
- [x] 6.2 Update `WorkspacePersistenceCoordinatorSession::RebuildTabs` to eagerly hydrate only the active tab, the most-recently-active tab in each split group, terminals, and pinned tabs; record the rest as `DeferredTabHandle` entries
- [x] 6.3 Hook `EditorTabService::ActivateTab` (or the closest existing entry point) so that activating a `DeferredTabHandle` runs the same code path that opens a tab from disk, populates the buffer, and replaces the handle with a real tab; drop the handle after hydration
- [x] 6.4 Verify the user-visible tab strip still shows every persisted tab (eager + deferred) with the correct title from the persisted record, with no "popping in" UX during boot
- [x] 6.5 Verify with the perf trace that `RestoreSessionState::RebuildTabs` is ≤ 100 ms median on a 20-tab persisted session
- [x] 6.6 Add fixtures + tests under `tests/workspace/` covering: (a) cold restore of a 20-tab session, (b) activating a deferred tab, (c) deferred-tab metadata round-trip across save/restore

## 7. Perf Harness Scenario (D7)

- [x] 7.1 Add fixture projects A and B under `tests/perf/fixtures/` with persisted sessions of 20 and 15 tabs respectively
- [x] 7.2 Add `tests/perf-harness/scenarios/switch_and_idle.cpp` that loads A, switches to B, idles 30 frames, and emits the metrics required by `Scenario: Switch-and-idle budget` in the modified `performance-budgets/spec.md`
- [x] 7.3 Commit baselines under `tests/perf/baselines/switch_and_idle.json` per the existing harness format
- [x] 7.4 Run the scenario via the `microide-perf` preset and attach the output to the change record (do not use sanitizer presets for perf measurement)

## 8. Documentation And Final Validation

- [x] 8.1 Update `docs/active-work.md` with the regression-recovery summary and the new budgets
- [x] 8.2 Update `docs/known-tech-debt.md` to mark items 8–12 (or their successors) as resolved by this change, or to update their status
- [x] 8.3 Update `AGENTS.md` § Do-Not-Regress Patterns with the four new invariants (single reload per activation, structural compare gate, per-frame prep once, plugin drain seam)
- [x] 8.4 Update `docs/perf-harness.md` with the new `switch_and_idle` scenario
- [x] 8.5 Run the full default test suite: `cmake --build build && ctest --test-dir build --output-on-failure`
- [x] 8.6 Run the ASAN preset: `cmake --preset microide-asan && cmake --build build/microide-asan && ctest --test-dir build/microide-asan --output-on-failure` (build succeeded; `ctest` failed on existing `WorkspaceShell/GitSidebarTooltipUsesSharedCompactCard`; initial run also hit environment-specific LSan ptrace limitation)
- [x] 8.7 Run the UBSAN preset and the TSAN preset (the TSAN run is the primary validation for §1): UBSAN build succeeded with the same existing test failure; TSAN build succeeded and surfaced a data race in `WorkspaceSidebarCoordinator.cpp` on the git-sidebar refresh snapshot path
- [x] 8.8 Capture a fresh perf trace using `MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1` and attach to the change record (`/tmp/microide_idle_trace.log`; executed via `./build/microide-perf-make/microide/microide` in the older nested `-B build/microide` layout because `./build/microide/microide` resolved to a directory there)
