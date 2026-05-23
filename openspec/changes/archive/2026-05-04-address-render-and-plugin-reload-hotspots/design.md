## Context

`feat/tech-debt-4` is the branch directly after the 2026-04-29 `comprehensive-tech-debt-cleanup`. That cleanup intentionally introduced narrower service interfaces (`EditorTabService`, `ProjectCatalogService`, `PluginRuntimeService`, etc.) and a `RenderViewModelBuilder` so render TUs no longer reach into shell context. Most of those changes are net-positive. But the perf trace in `proposal.md` shows several places where the new boundaries either:

1. **double-call** an expensive operation that used to be called once (the project-switch plugin reload),
2. **lose a guard** the older monolithic path had implicitly (the compare-surface render runs without checking the active surface in the partial-clip path),
3. **broaden invalidation** because the new flag plumbing always passes `reload_syntax_definitions = true` rather than letting the runtime decide,
4. **pull whole-workspace work into a per-clip loop** because `RenderPrepared` is now invoked per clip rect by `Application::WorkspaceRender(partial-clip)`, and
5. **defer no work** during session restore — every persisted tab is fully materialised during boot.

A separate concern from the user: the session ended in a plugin-related crash. The trace itself doesn't carry the stack, but the plugin reload sequence visible in the trace (`PluginRuntimeReload` running twice within ~1 s during a project switch, with async callbacks active) is exactly the kind of pattern that would expose a use-after-free if shutdown doesn't drain the plugin host's worker thread.

Constraints this design must respect:

- The 2026-04-29 hard architectural invariants in `CLAUDE.md` and `tests/ArchitectureInvariantsTests.cpp` (no `WorkspaceShell&` on coordinator constructors; no `friend` in `src/workspace/*`; render TUs can't read `context_.current_project_state` directly; `WorkspaceShell.h ≤ 400` lines, `.cpp ≤ 600`).
- `correctness > speed > low-CPU > low-memory` priority order (per `AGENTS.md`).
- Plugin host stays decomposed; `lua_State*` only behind `plugin/LuaRuntime`; no `src/plugin/*.cpp` translation unit > 800 lines.
- Single-PR scope: the change must not require a follow-up to be safe to ship.

Stakeholders: anyone using the workspace boot path (everyone), plugin authors (shutdown contract), and the perf-harness CI.

## Goals / Non-Goals

**Goals:**

- Restore startup time on the user's session to ≤ 250 ms `WorkspaceShell::InitializeCurrentProject` (was 744 ms; bound dominated by `RebuildTabs` at 691 ms).
- Project switch ≤ 60 ms median, ≤ 120 ms P95 (was 142 ms with double reload + full syntax invalidation).
- Idle partial-frame `Application::Render(partial)` for a 1-dirty-rect 1-clip frame ≤ 2.5 ms median (was ~5 ms).
- Eliminate `RenderCompareSurface` cost when no compare/merge tab is active (was 7–13 ms per partial frame, target 0).
- Eliminate the second back-to-back plugin reload on project switch.
- Plugin host shutdown is deterministic: no callback fires after `LuaRuntime::Shutdown` returns; `Reload` joins worker thread before tearing down plugins. Add ASAN/TSAN regression coverage.
- Add a perf-harness scenario that replays "open → switch → idle" so this regression class is detectable in CI.

**Non-Goals:**

- Re-architecting `RenderViewModelBuilder` or the partial-clip pipeline beyond the prep/scoping fixes called out here. The view-model builder shape itself is correct; we are removing whole-workspace work that should not be inside the per-clip loop.
- Changing the persistence file format (`PersistedRecordReader/Writer`) — restoration becomes lazier, but the on-disk schema is unchanged.
- Re-writing the plugin-host threading model. We are adding deterministic drain/join, not introducing a new executor.
- LSP startup behaviour. The earlier `b15753f` commit already removed LSP stalls from project/tab lifecycle; this change does not touch LSP.
- Memory regressions: the lazy tab restore keeps a small amount of additional state per deferred tab. We accept the trade given the priority order (correctness > speed > low-CPU > low-memory).

## Decisions

### D1. Single plugin reload per `ActivateProjectState`

**Decision:** Collapse the two reload sites in `ProjectCatalogService::ActivateProjectState` (`src/workspace/ProjectCatalogService.cpp:78` via `initialize_current_project`, and `:101` unconditional) into one call. The `bool` parameter on `reload_plugins_for_current_project` becomes a `struct PluginReloadRequest { bool syntax_definitions; bool replay_buffer_opens; bool open_lsp_documents; }` so callers state intent once.

**Rationale:** The trace shows `initialize_current_project` already runs `ReloadPluginsForCurrentProject` once (8 ms) on the first activation. The second call at `:101` is a guard against the "already initialised, just reactivate" path — but in that branch the project root hasn't changed, so a full plugin reload is the wrong action. What the reactivation actually needs is `RefreshPluginSidebar` + selection normalize, not `LuaRuntime::Reload` + syntax-cache invalidation. Splitting the reactivation into a smaller `RefreshPluginSurfacesForReactivation` call keeps the cheap path cheap.

**Alternatives considered:**

- Add a "skip if root unchanged" check inside `ReloadPluginsForCurrentProject`. Rejected — pushes the policy decision into a leaf method and hides the cost; the existing trace name still fires.
- Keep both call sites and just pass `reload_syntax_definitions=false` on the second. Rejected — still tears down + reloads `LuaRuntime`, still does ~12 ms of work the reactivation doesn't need.

### D2. Scope syntax-cache invalidation to actually-changed languages

**Decision:** Replace the boolean `syntax_definitions_changed()` query with a `ChangedSyntaxLanguages()` set returning the language IDs whose definitions differ from the previous load. `InvalidateRuntimeSyntaxStateCaches` takes the set and only invalidates the per-tab cached tokens for tabs whose buffer language is in that set. Empty set → no work.

**Rationale:** `InvalidateRuntimeSyntaxStateCaches` is currently 107 ms for an N-tab session because it walks every tab and rebuilds its tokenization. In the trace, the project switch did not actually change the plugin set in a way that affected most tabs' languages — the boolean was set conservatively. Scoping to changed languages turns the worst case (all tabs) into the typical case (zero tabs touched).

**Alternatives considered:**

- Keep the boolean and just lazily invalidate per-tab on next render. Rejected — moves the cost into the partial-clip render path, which is exactly where we are *removing* whole-workspace work.

### D3. Lazy tab rehydration during `RestoreSessionState`

**Decision:** `WorkspacePersistenceCoordinatorSession::RebuildTabs` (`src/workspace/WorkspacePersistenceCoordinatorSession.cpp:234-474`) materialises only:

- the active tab,
- the most-recently-active tab in each split group, and
- any tab pinned (`tab.pinned`).

All other persisted tabs are recorded as `DeferredTabHandle` (path, language hint, viewport scroll, selection range) and stored on the tab strip. On first activation of a deferred tab — which goes through `EditorTabService::ActivateTab` — the handle is hydrated via the same code path that opens a tab from disk today, then dropped. This piggy-backs on existing buffer-open infrastructure without a parallel restore path.

**Rationale:** The 691 ms cost is dominated by per-tab `LoadContent()` + editor preference application + split-tree reconstruction. The user's typical session has many tabs open but only interacts with a handful at a time; deferring 80% of them eliminates 80% of the cost while staying transparent — the tab strip still shows all tabs with their persisted titles.

**Alternatives considered:**

- Parallel restore (thread pool). Rejected — `LoadContent` touches the buffer registry which is host-owned; threading it requires more invariant work than the pass justifies.
- Stream restoration onto the main loop after first paint. Rejected — adds visible "tabs popping in" UX and complicates the redraw policy.

### D4. Compare-surface render gating in the partial-clip path

**Decision:** Move the `ActiveTabIsCompare()` gate from `RenderActiveWorkspaceSurface` (the early-out at `WorkspaceShellCompareRender.cpp:173`) up into the view-model builder, so the render-frame view model literally does not contain a compare entry when there is no compare tab active. The render TUs that consume the view model then have nothing to do — the gate becomes structural rather than runtime.

**Rationale:** Today a stale compare tab in another split (or stale state after a tab close) can leave `RenderActiveWorkspaceSurface` running the compare path during a partial-clip frame even though the dirty rect is somewhere else. The trace shows 7–13 ms `RenderCompareSurface` running on partial frames where the user clearly isn't on the compare surface. Making absence-of-compare a property of the view model makes regressions catchable by the architectural-lint test (`tests/ArchitectureInvariantsTests.cpp`) the same way the 2026-04-29 invariants are enforced.

**Alternatives considered:**

- Add a defensive `if (!compare_active) return;` to every entry point in `WorkspaceShellCompareRender.cpp`. Rejected — guards are easy to add, easy to remove; an absent view-model field is harder to misuse.

### D5. Move per-frame whole-workspace prep out of the per-clip loop

**Decision:** `Application::WorkspaceRender(partial-loop ... clip rects from ... dirty rects)` currently iterates and calls into render code per clip rect; `WorkspaceShell::PrepareRenderFrame` work (mouse-state sync, layout recompute, split-tree normalize) is whole-workspace and must run **once per frame**, not per clip. Split into:

- `WorkspaceShell::PrepareFrameOnce(FrameContext)` — called by `Application::WorkspacePrepareFrame` exactly once per frame (full or partial), regardless of clip count.
- `WorkspaceShell::RenderClip(ClipRect, ...)` — called per clip rect, scoped strictly to the clip.

The 2026-04-29 invariant that render TUs consume `RenderViewModelBuilder` view models is preserved; the view model is built once during `PrepareFrameOnce`.

**Rationale:** Even with 1 dirty rect / 1 clip, today's partial-clip path costs ~5 ms because `RenderPrepared` re-runs whole-workspace work. Moving it to once-per-frame removes the duplication and gives the per-clip body roughly the cost of the actual draw — which the trace suggests is around 1–2 ms.

**Alternatives considered:**

- Cache per-frame-prep results inside `RenderPrepared` and short-circuit on second call. Rejected — caching an implicit "once per frame" contract is exactly the kind of hidden coupling `CLAUDE.md` warns against.

### D6. Deterministic plugin-host shutdown / reload drain

**Decision:** `LuaRuntime::Shutdown` and `LuaRuntime::Reload` both:

1. Mark the runtime as draining (existing `CancelCallbacks` behaviour stays).
2. Wait synchronously for the plugin host's background worker(s) to observe the cancel and finish in-flight work — bounded by a deadline (e.g. 250 ms; if exceeded, log a warning and continue, but in normal operation it returns immediately).
3. *Then* call `TearDownPlugins`.

Implementation is a `std::condition_variable` + counter on `async_process_state` ticked by each worker on completion of a unit of work. `async_process_state` keeps its `shared_ptr` lifetime, but no callback can be in-flight when teardown begins. Add a TSAN regression test that interleaves rapid `Reload` calls under load.

**Rationale:** The user reported a plugin-related crash. The current code path cancels then tears down without joining; under the project-switch double-reload pattern (which D1 also fixes), a worker that picked up a pre-cancel callback can outlive its plugin instance. Even after D1 collapses the double reload, the lifetime hole exists for any future code that calls `Reload` while plugins are working.

**Alternatives considered:**

- Track per-callback weak references and let dangling callbacks no-op. Rejected — this is the pattern that enabled the crash; weakening it further doesn't make it safer, just harder to diagnose.

### D7. Perf-harness regression scenario

**Decision:** Add `tests/perf-harness/scenarios/switch-and-idle.cpp` (or extend an existing scenario file, per `dev-docs/performance/perf-harness.md`) that:

1. Loads a fixture project A with 20 tabs persisted.
2. Switches to fixture project B with 15 tabs persisted.
3. Idles 30 frames.
4. Asserts wall-clock budgets matching the new budgets in `performance-budgets/spec.md`.

Run from `microide-perf` preset only; not gated in default CI but available via a focused target so this regression is reproducible on demand.

## Risks / Trade-offs

- **Lazy tab restore changes timing of side effects.** Some tab-open side effects (LSP `textDocument/didOpen`, file-watch registration) currently fire during restore for all tabs. Deferring them to first-activation means a tab the user never opens this session never registers — that's the *intended* behaviour but plugin authors who relied on "restored == loaded" need to migrate. Mitigation: explicit changelog entry; lint test that flags any plugin host code that assumes restore implies open.
- **Single-reload contract narrows what reactivation can change.** If a plugin's syntax definitions are modified between sessions while the project state is already initialised in memory, the reactivation no longer picks them up. Mitigation: `EditorTabService` exposes an explicit `RequestSyntaxReload(language_id)` that callers (e.g. plugin reload commands) can invoke; reactivation itself stays cheap.
- **Per-frame-once split risks misuse.** A future caller that forgets to invoke `PrepareFrameOnce` before `RenderClip` will produce a stale view model. Mitigation: bake the contract into a `FrameToken` that `RenderClip` requires by signature (RAII-style, can't be default-constructed).
- **Plugin shutdown drain may regress shutdown latency by tens of ms** if a worker is mid-callback. Acceptable trade vs. crash risk, and the 250 ms deadline caps worst case.
- **Compare-surface view-model gating** changes the contract between `RenderViewModelBuilder` and the compare render TU. We update the architectural-lint test to enforce it; this is the right kind of gate per `CLAUDE.md`'s rendering-stays-host-owned principle.
- **Measurement bias:** all targets are measured on the default `build/` tree (or `microide-perf` for harness). Sanitizer presets (`microide-asan/-ubsan/-tsan`) are not used for budget validation — they are only run for the D6 plugin shutdown change to validate the lifetime fix.

## Migration Plan

This is a single PR, no staged rollout. Order of work follows `tasks.md`:

1. D6 first (correctness — removes the crash class) so subsequent perf changes don't ship on top of a broken plugin lifetime.
2. D1 + D2 together (project-switch path).
3. D4 + D5 (render path).
4. D3 last (boot path; depends on D1 stability).
5. D7 perf-harness scenario landed alongside the budget update.

Rollback: revert the PR. No data migration, no on-disk format change. Plugin authors who consumed undocumented "fire-and-forget callback survives shutdown" behaviour will see the warning log if a callback exceeds the drain deadline.

## Open Questions

- `D3 lazy restore`: should the deferred-tab handle keep the persisted scroll/selection in memory, or re-read from the session file on hydration? Memory says "in memory"; correctness says "re-read so a session-file edit between restore and hydration wins." Default to in-memory since session-file is not user-edited and the size is trivial; revisit if surfacing.
- `D5 frame-once split`: does `WorkspaceShell::PrepareRenderFrame` have any work that *should* run per clip (clip-bounded mouse hit-test, etc.)? The instinct is no, but the implementation must audit each line and document where it ends up.
- `D6 drain deadline`: 250 ms is a guess. If the harness shows real plugin work commonly takes longer, raise to a tier (250 ms warn, hard cap higher) — but don't ship without an upper bound.
