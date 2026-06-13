# AGENTS

## Mission

Build `microide` as a native, low-footprint, single-window C++/SDL3 desktop IDE. The target shape
is: no GPU acceleration requirement, keyboard-first workflows, and a well-validated diff / merge /
git workstation around a competent editor, search, and terminal core.

Avoid marketing claims in code, commits, or docs. The project has internal regression baselines
but no comparative benchmarks against other editors; do not write "fastest" / "lower CPU than X"
phrasing — say "native, low-footprint, responsive" and let measurements speak. See the
"Performance & Benchmark Methodology" section of `README.md` for what is actually measured.

Do not optimize for keeping old boundaries alive. If the correct fix breaks compatibility, touches
many files, or requires a broader refactor, prefer the better design.

## Product Pillars

MicroIDE's durable built-in capabilities are:

- **Editor** — text editing, UTF-8, IME, undo/redo, syntax highlighting, blame, splits
- **Diff & Merge** — compare tabs (HEAD, commits, outgoing), three-way merge tabs, shared
  row-decoration pipeline; see `openspec/specs/diff-merge-editor/spec.md` for the contract
- **Search** — async project search (literal + regex), replace-in-project, file finder
- **Git** — working-tree changes, staging, conflicts, outgoing files, blame
- **Terminal** — PTY-backed tabs with scrollback, alternate screen, full ANSI support
- **Plugins** — Lua 5.4 runtime with narrow host-owned registries for commands, sidebars,
  settings, keybindings, status items, diagnostics, hover, formatters, tasks, tools, tests,
  SCM, and auth

The authoritative product thesis, priority order, and non-goals live in
`openspec/specs/product-vision/spec.md`.

## Priority Order

When tradeoffs conflict, use this order:

1. correctness
2. speed
3. low CPU usage
4. low memory usage
5. maintainability and simplicity
6. compatibility only when it is explicitly required

Compatibility is not a default constraint. Internal APIs, temporary abstractions, and stale call
patterns can be broken or removed if that is the cleanest way to improve the system.

## Default Engineering Stance

- Prefer correct behavior over minimal diffs.
- Prefer cohesive refactors over local patches that preserve bad architecture.
- It is fine to touch many files when the change crosses ownership boundaries in reality.
- Delete dead code, stale docs, and temporary compatibility shims as soon as the new path is established.
- Keep the host small and explicit; registries and services are better extension points than giant mutable objects.

## Plugin Rules

- Do not expose `WorkspaceShell` wholesale to plugins.
- Prefer host-owned registries, commands, sidebars, and services over ad hoc plugin hooks.
- Keep rendering host-owned; plugin contributions should provide data, commands, or structured requests.
- Add async plugin execution only when real plugin workloads justify it.
- If plugin work reveals that a subsystem boundary is wrong, fix the boundary instead of layering a compatibility adapter over it.

## Performance Rules

- Speed is the main optimization target after correctness.
- CPU comes before memory, especially idle CPU and redraw-path CPU.
- Measure before and after performance-sensitive changes.
- Use `dev-docs/performance/perf-harness.md` scenarios and baselines as the primary regression oracle.
- Keep `microide_perf_tests` green locally for touched scenarios and treat `perf-runner-v1` baseline checks as the authoritative gate.
- Use `dev-docs/performance/startup-tracing.md` and `dev-docs/performance/runtime-profiling.md` instead of guessing.
- Preserve typing, scrolling, resize, and startup responsiveness even when adding features.
- Prefer deleting redundant work over caching everything by default.
- The durable performance budget contract lives in `openspec/specs/performance-budgets/spec.md`.

## Architecture Rules

- Keep ownership narrow and obvious.
- Prefer small focused translation units over catch-all "shared" files.
- Push external tool and OS integration behind `src/project/*` or similarly narrow service boundaries.
- Keep UI orchestration thin; deterministic logic belongs in testable helpers.
- Avoid hidden coupling through mutable global state.
- If a coordinator grows because a subsystem lacks a real API, add the API and move logic out.

## Do-Not-Regress Patterns

The 2026-04-29 `comprehensive-tech-debt-cleanup` (archived under
`openspec/changes/archive/2026-04-29-comprehensive-tech-debt-cleanup/`) removed several patterns
that had previously grown back. Do not reintroduce any of them. The architectural-lint test
(`tests/ArchitectureInvariantsTests.cpp`, registered as `ArchitectureInvariants/SoftChecks` in the
default `ctest` run) hard-fails on most of these; the rest are policy and reviewer-enforced.

Hard-fail invariants (lint will reject the change):

- No `friend class` or `friend struct` in `src/workspace/*`. Use a service interface or an
  explicit narrow accessor instead.
- No `WorkspaceShell&` or `WorkspaceShell*` parameters in any
  `src/workspace/Workspace*Coordinator*.h` constructor. Inject the specific service interface
  (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`,
  `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `PersistenceService`) or
  a small callback bundle.
- No `try`/`catch` around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, or `std::stod`.
  Use the non-throwing `util/Parse.h` helpers (`ParseInt`, `ParseInt64`, `ParseSize`,
  `ParseFloat`) and handle `std::optional` directly.
- `src/workspace/WorkspaceShell.h` ≤ 400 lines and `src/workspace/WorkspaceShell.cpp` ≤ 600 lines.
  Add behavior to a service, not the shell.
- The render translation units covered by `CheckRenderSurfaceStateAccess`
  (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`,
  `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`,
  `WorkspaceShellHoverTargets`) consume view models built by `RenderViewModelBuilder`. Do not read
  `context_.current_project_state` or call `CurrentTextInputSurface(...)` from these files.

Policy invariants (no automated lint, but reviewers will reject):

- No project-level or shell-level fallback editor viewport. Resolve the active editor target
  through `EditorTabService::ActiveViewport()` (or equivalent typed accessor). The legacy symbols
  `text_viewport_` and `current_project_state_.text_viewport` were deleted intentionally; do not
  reintroduce equivalents under a new name.
- No bespoke per-section parser for project state, user config, and session restore.
  These artifacts route through `PersistedRecordReader`/`PersistedRecordWriter`
  and `PersistenceService`. Add a typed record, do not hand-roll a text format.
- No new direct file I/O for workspace/session/config/conversation state outside
  `PersistenceService`.
- No per-surface duplicate of single-line edit operations (insert, backspace, delete-forward,
  caret movement, selection, copy, cut, paste, select-all). Single-line surfaces consume
  `editor/SingleLineEditor.{h,cpp}` plus `editor/SingleLineKeyHandler.{h,cpp}`.
- No `lua_State*` outside `plugin/LuaRuntime.{h,cpp}`. Plugin extension-surface modules consume
  the runtime through opaque handles.
- Plugin extension surfaces stay split across focused translation units (commands, sidebars,
  syntax, diagnostics, hover, providers, lifecycle); do not collapse them back into a monolithic
  `PluginHost.cpp`. Each `src/plugin/*.cpp` translation unit stays at or below 800 lines.
- View models do not hold pointers or references to `WorkspaceShell`, coordinators, or services.
  They are POD-like structs populated by the builder.
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay
  host-owned and service-backed. Menu overflow, status-bar actions, Settings, and
  Help/About must route through action/service state and `RenderViewModelBuilder`, not plugin-owned
  rendering or render-TU product logic.
- Project reactivation paths do not reload plugins. Reactivation refresh uses
  `refresh_plugin_surfaces_for_reactivation` and must not call
  `ReloadPluginsForCurrentProject`.
- Compare-surface rendering is structurally gated by `RenderViewModelBuilder` output. Render units
  must not inspect shell or project state to decide whether compare content renders.
- Per-frame preparation work executes once per frame. Per-clip entry points must not rebuild frame
  layout, normalize state, or rebuild render view models.
- Plugin reload/shutdown must drain async process workers before plugin teardown using the bounded
  drain seam; do not call teardown directly without the drain step.

The 2026-05-02 `deferred-work-and-throughput-pass` added four further invariants:

- No synchronous subprocess wait on the main thread in workspace code (`src/workspace/`). New code
  that needs subprocess results must dispatch through `ProjectBackgroundExecutor` or a similar
  background executor and deliver results via SDL user event. The
  `CheckNoSynchronousSubprocessWaitInWorkspace` lint rule hard-fails on direct `Subprocess::Wait`,
  `waitpid`, `WaitForSingleObject`, or `WaitForMultipleObjects` calls in workspace TUs.
- LSP `textDocument/didOpen` and `textDocument/didChange` notifications must not be sent
  synchronously on the `ActivateTab` call path. The hydration notification must be posted
  asynchronously so the tab is visible before any LSP acknowledgement is awaited.
- `ComputeLayout()` must be skipped when the layout-dirty flag is clear. Layout recomputation must
  only run when a window-resize, divider-drag, sidebar-toggle, or panel-toggle event has set the
  dirty flag; do not call it unconditionally from `PrepareFrameOnce`.
- The SDL event loop must never use a zero-delay `SDL_PollEvent` spin at idle. Use the
  `IdleHint`-driven strategy: `Full` hint → `SDL_PollEvent`; `CaretOnly` hint →
  `SDL_WaitEventTimeout(caret_remaining_ms)`; `Idle` hint → `SDL_WaitEvent`.

The 2026-05-06 `codebase-cleanup-perf-and-debt` adds four further hard invariants:

- No legacy persistence symbols (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`,
  `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`,
  `ParseProjectSessionText`, `ParseWorkspaceSessionText`) anywhere in `src/`, `tests/`, or
  `tools/`; this prevents accidental resurrection of the deleted importer path.
- No `platform::RunSubprocess(...)` calls in `src/workspace/*.cpp`; workspace subprocess work must
  route through `ProjectBackgroundExecutor` to keep shell-thread latency predictable.
- Render translation units (`WorkspaceShellRender*.cpp`) must not materialize new strings in hot
  paths; string assembly belongs in `RenderViewModelBuilder` so per-frame rendering stays lean.
- `TextViewport` non-const mutation paths must not copy `document_->lines` wholesale; undo and
  edit flows should capture only the affected ranges to avoid large-buffer copy regressions.

The durable contracts these rules implement live in
`openspec/specs/workspace-architecture/spec.md`, `openspec/specs/persisted-state-format/spec.md`,
and `openspec/specs/shared-edit-primitives/spec.md`. Update those specs in the same change when a
durable invariant moves.

## Testing Rules

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before committing.
- Prefer `tools/run-checks.sh {tests|asan|ubsan|tsan|all}`, which tees build+test output to `/tmp/microide-<target>.log`; read that file instead of rerunning.
- Run sanitizer variants (`microide-asan`, `microide-ubsan`, `microide-tsan`) for memory/thread-sensitive changes.
- TSAN runs require `sudo sysctl vm.mmap_rnd_bits=28` on Linux before test execution.
- Extend and run relevant fuzz targets in `tests/fuzz/` when changing persistence, parser, regex, or blame decode paths.
- Redraw comparison tests under SDL dummy video should run serially.
- Use focused fixtures for git, search, compare, merge, and plugin-adjacent workflows.
- If a change is hard to test, treat that as a design smell and improve the seam.

## Documentation Rules

- Keep `dev-docs/project/active-work.md` current when priorities or shipped status change.
- Keep `dev-docs/project/implementation-guide.md` aligned with durable product direction.
- Update subsystem design docs when a change materially alters the intended architecture.
- Remove stale or split-brain docs rather than leaving contradictory guidance around.
- When a durable policy changes, update the relevant `openspec/specs/` file in the same commit.

## Iteration Loop

The default loop is:

1. implementation
2. tests
3. docs
4. commit

Repeat in coherent slices. Do not leave tests, docs, or commits as an afterthought.

## Commit Discipline

- Prefer coherent commits over giant mixed snapshots.
- Each commit should represent a defensible step forward.
- If the right fix is broad, make it broad but still keep the commit logically unified.
- Mention major refactors plainly in the commit message instead of hiding them behind vague wording.
