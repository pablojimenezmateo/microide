# Agent Guide

Purpose: first-stop operating guide for agents working in this repository.

## Quick Scan

- `microide` is a native desktop IDE shell built in C++20 with CMake and SDL3.
- The current priority is speed first, then correctness, then low CPU usage, then low memory. Speed leads because latency is the product; correctness still outranks CPU/memory/clarity, so a fast path that is wrong on a routine input is not a valid solution. See `AGENTS.md` § Priority Order.
- Plugin support is an active expansion phase; keep plugin seams narrow and host-owned.
- `AGENTS.md` owns repo policy, `dev-docs/project/active-work.md` owns current direction, and `dev-docs/project/implementation-guide.md` owns the durable product map.
- Build with `cmake`, test with `ctest`, and use focused `microide_tests` filters for quick validation.
- Prefer explicit ownership, deterministic helpers, and thin coordinators over broad mutable facades.

## Project Context

`microide` is a compact single-window desktop editor and IDE shell. The codebase currently centers on built-in editor, compare, merge, search, git, terminal, and plugin workflows.

### Where things live in `src/workspace/`

`src/workspace` is the largest subsystem (~58% of the tree). It is split by what a
file *is*, first match wins — subsystem beats role, role beats catch-all:

| directory | holds |
| --- | --- |
| `shell/` | `WorkspaceShell` + its companion TUs (the thin orchestrator and its per-area entry points) |
| `render/` | every `WorkspaceShellRender*` TU, `RenderViewModelBuilder`, and the other view-model-only paint units (`DebugPaneRender`, the hover TUs) |
| `coordinators/` | `Workspace*Coordinator*` — including a coordinator's continuation TUs |
| `services/` | `*Service` host-owned service boundaries |
| `registries/` | `*Registry` contribution/lookup tables |
| `actions/` | `ActionId` types, requests, executors, dispatch |
| `state/` | `Workspace*State` models |
| `assist/`, `lsp/`, `debug/`, `git/`, `persistence/`, `control/` | subsystem-owned clients, protocols, models, and presentation |
| top level | shared workspace vocabulary every bucket includes (`WorkspaceContext`, `WorkspaceLayout`, `WorkspaceUiText`, `FileUri`, …) |

Put a new file in the directory that matches what it is. The architecture lint
iterates `src/workspace` **recursively** and selects files by filename, so a
correctly-named file is covered wherever it sits — but a rule that names an
explicit path must be repointed when a file moves, and `RequireRuleTarget` /
`ReadRuleTarget` will hard-fail the run if you forget rather than going quietly
green. Use those helpers for any new path-named rule.

## Source Of Truth

When guidance conflicts, use this order:

1. `AGENTS.md`
2. `openspec/specs/` — authoritative product contracts (vision, diff/merge, performance budgets)
3. `dev-docs/project/active-work.md`
4. `dev-docs/project/implementation-guide.md`
5. Focused subsystem docs in `dev-docs/`
6. The handbook under `guidelines/`

The list above orders *durable contracts* (specs over docs). It is not a recency
order: `dev-docs/project/active-work.md` is the single source of truth for the
**current shipped baseline and active priority stack**, so for "what is true /
what to do right now" decisions it supersedes the forward-looking material in
`openspec/specs/` and the roadmap. Consult active-work.md first when a spec or
roadmap entry describes intended rather than shipped state.

## Agent Best Practices

- Start by narrowing the problem with fast repo inspection. Prefer `rg`, `rg --files`, `sed -n`, `git show`, and targeted reads over broad dumps.
- Match the repo's design direction instead of preserving stale boundaries. Broad refactors are acceptable when they improve correctness or subsystem ownership.
- Keep rendering host-owned. Plugins should contribute data, commands, providers, or structured requests, not raw shell internals.
- Avoid growing `WorkspaceShell` as a catch-all object. If work wants shell access, look for or add a narrower registry, coordinator, or service boundary.
- Prefer RAII, explicit ownership, and value semantics. Reach for inheritance only when there is a durable polymorphic boundary; otherwise prefer plain types, composition, and focused helpers.
- Keep deterministic logic out of SDL event glue and paint code when possible. Thin orchestration layers are easier to test and safer to refactor.
- Avoid hidden coupling through mutable global state, shared singleton-style services, or broad friend access.
- Treat performance-sensitive work as measurable engineering. Use `dev-docs/performance/startup-tracing.md` and `dev-docs/performance/runtime-profiling.md` instead of guessing.
- Do not write comments in source code. Make the code say it through names, types and small functions; put the why in the commit message or `dev-docs/`. The only comments allowed are tool directives: `NOLINT`, `IWYU pragma`, and the perf harness's `perf-measure-builds-input:` / `perf-measure-waits-on-clock:` exemptions.

## Development Workflow

Full guide, with the reasoning behind each lane: `dev-docs/project/development-workflow.md`.

```bash
cmake -S . -B build && cmake --build build -j8          # configure + full build
# inner loop: the two test binaries only, and only their shards
cmake --build build --target microide_tests microide_kernel_tests -j8
ctest --test-dir build -R _tests_shard --output-on-failure -j$(nproc)
./build/microide/microide_tests "WorkspaceShell/EditorDirty"   # focused filters (substrings)
```

- Two test binaries: `microide_kernel_tests` (the SDL-free kernel: util, platform,
  project, compare, persistence, terminal; links `microide_kernel` alone) and
  `microide_tests`. A kernel test that needs SDL or the shell moves to
  `microide_tests`. `-R microide_tests_shard` alone skips every kernel test.
- A plain `ctest` after the narrow build runs STALE perf/probe binaries and reports
  green. Before concluding anything: `tools/run-checks.sh tests`, which builds every
  target ctest invokes. ccache and ld.lld are used automatically when installed.
- `tools/run-checks.sh <lane>` tees to `/tmp/microide-<lane>.log` (or
  `$MICROIDE_LOG_DIR`); read the log instead of rerunning. Lanes: `tests`, `asan`,
  `ubsan`, `tsan`, `perf-tests` (arms the allocation counters -- run after touching a
  render, view-model or presentation hot path), `perf-canary` (proves the perf gate
  can fail), `clang-build` (second compiler, warnings as errors -- run after adding a
  source to a curated target list), `hardened` (`_GLIBCXX_ASSERTIONS`), `tidy`, `qa`
  (GUI QA on Xvfb with the redraw verifier -- run after touching damage, redraw,
  hover, focus or paint), `qa-asan`, `qa-remote`, `qa-wayland`, `qa-scale`,
  `wake-race`, `all`. `tools/check-invariants-bite.sh` proves every recorded negative
  control still fails its check.
- Sanitizer and fuzz recipes (presets, `setarch -R` for TSan, LSan/TSan
  suppressions, fuzz corpora) are in the full guide.
- The QA monkey runs continuously on the dev PC: `tools/soak/soak.py run` (a
  `systemd --user` unit) follows `main`, and `tools/soak/soak.py status` / `report`
  read its findings ledger (`build/soak`, a `FINDING.md` per finding); `soak.py
  health` says whether every cell runs and what the soak does not reach (the
  surface's gap list, the source coverage: `soak.py coverage`).
  `tools/soak/AGENT.md` is the brief for fixing what it finds -- for a finding
  no replay reproduces, from its `rr` recording (`soak.py hunt SIG` makes one).
  The app's QA hooks (`invariants`, `crash-points`, the crash backtrace and
  stall watchdog) are in `dev-docs/control/control-channel.md`.
- Performance work: `dev-docs/performance/perf-harness.md`, then the startup/runtime
  tracing docs.

## Architecture Stance

- Correctness beats compatibility unless compatibility is explicitly required.
- Plugin expansion should tighten host boundaries, not widen ad hoc shell access.
- Rendering, redraw policy, layout, and shell chrome remain host-owned.
- External tools and OS integration should stay behind narrow services in `src/project/*`, `src/platform/*`, or similarly focused modules.
- Coordinators should translate input or intent into state changes and service calls; they should not become long-lived ownership sinks.
- If a subsystem boundary is wrong, fix the boundary rather than adding a compatibility shim around it.

## Hard Architectural Invariants

Do not reintroduce what these rules removed. Each names the check in
`tests/ArchitectureInvariantsTests.cpp` / `tests/architecture/` that hard-fails it
(fixtures both ways in `tests/architecture/ArchitectureRuleFixtures.cpp`), or says it
is reviewer-enforced. The reasoning behind each -- the bug that made it a rule -- is
in `dev-docs/project/architecture-invariants.md`; read it before relaxing one. A new
lint needs fixtures both ways and a loud missing-target guard (`RequireRuleTarget` /
`ReadRuleTarget`); see `dev-docs/project/validation-traps.md`.

Shell and coordinators
- Coordinator constructors take service interfaces, never `WorkspaceShell&`/`*`. `CheckCoordinatorShellConstructors`
- No `friend` in `src/workspace/*` except the `*TestAccess` backdoor. `CheckWorkspaceFriends`
- The shell is a thin orchestrator: `WorkspaceShell.h` <= 400 and `WorkspaceShell.cpp` <= 600 code lines, `WorkspaceShellMembers.inc` under a cap that ratchets DOWN (read it from the test, never from memory), companion TUs capped at 47. New behavior lands on a service. `CheckShellFileSize`, `CheckWorkspaceShellCompanionTuCount`
- `Make*Service()`/`Make*Coordinator()` results are bound by reference and never captured by value into a lambda. `CheckShellGlueIsBoundByReference`, `CheckFactoryResultsAreNotCapturedByValue`
- No `platform::RunSubprocess` and no synchronous subprocess wait in workspace code; dispatch through `ProjectBackgroundExecutor`. `CheckNoSynchronousSubprocessInWorkspace`, `CheckNoSynchronousSubprocessWaitInWorkspace`
- Overlays hide through `DismissOverlay` / `HideOverlay(state)`, never a bare `overlay.visible = false`. `CheckOverlayDismissalIsCentralized`
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay host-owned and service-backed. Reviewer.

Editor, tabs and panes
- The active editor viewport resolves through `EditorTabService::ActiveViewport()`; no shell- or project-level fallback viewport under any name. `CheckNoFallbackEditorViewportSymbols` (renamed revivals: reviewer)
- `EditorSplitTree`'s leaves are `editor_groups` in order: a TU that reshapes the groups calls a structural tree method; the cap is `kMaxEditorGroups` (8). `CheckEditorGroupsMutateWithTheSplitTree`
- Compare, merge, the find widget and the debug toolbar lay out in the FOCUSED pane (`ActiveTabSurface`), never `layout.editor_surface`. `CheckTabSurfacesLayOutInTheFocusedPane`
- `TextViewport` editing never snapshot-copies `document_->lines`; capture affected ranges only. `CheckTextViewportNoFullDocCopy`
- Filetype detection goes through `TextViewport::language_id()`; the content-reading `DetectFiletype(path, lines)` is reserved for that memo. `CheckViewportFiletypeGoesThroughTheViewportMemo`
- Same-language tabs share one `LanguageContractView` (`WorkspaceLanguageContract::ResolveEditorView`); never an owned view per tab. Perf scenario `settings_change_many_tabs`.
- Single-line inputs use `editor/SingleLineEditor` + `SingleLineKeyHandler`. Reviewer.

Rendering
- Render TUs consume view models from `RenderViewModelBuilder`; they never read `context_.current_project_state` or call `CurrentTextInputSurface`. `CheckRenderSurfaceStateAccess`
- View models own their data: no `OverlayState` in `RenderViewModelBuilder.h`, a `ProjectWorkspaceState*` only in `FrameSurfaceViewModel` / `SidebarSurfaceViewModel`, and no pointers to the shell, coordinators or services. `CheckRenderViewModelsOwnProjectState` (pointers to services: reviewer)
- Render TUs materialize no strings in hot paths. Compare/merge render TUs keep their `WorkspaceShellRender*` names, paint through the reused scratch rows and truncate through `TruncateLabelView`. `CheckRenderTuDoesNotMaterializeStrings`, `CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings`, `CheckCompareMergeRenderUsesScratchRows`
- Frame preparation runs once per frame; `ComputeLayout()` only when the layout is dirty; the idle loop never spins `SDL_PollEvent` at zero delay (`IdleHint`). Reviewer.
- Repaint damage is derived, not requested: a derived surface's `Prepare*Frame` lays out its units (a rect plus a fingerprint of everything that unit paints) and `DerivedDamage::Commit` damages what changed; its paint reads only that prepared snapshot. A change to a derived surface asks for a frame (`RequestPreparedFrame` / a `request_frame` operation), never a rect; a hand-written rect is only for what is not derived yet. Every surface commits every frame, shown or not. Every Chrome unit names its `ChromePainter`; `RenderClip` skips passes only by their prepared extents, and a pass drawing outside its extent is a `pass:` overrun. `CheckDerivedSurfacePaintReadsOnlyItsSnapshot` (body, parameter types, and the shell members it calls); key completeness: the QA redraw verifier, the `*RowDamageFollowsEveryStateTransition` property tests and `ASettledFrameDerivesNoDamage`.

Persistence, files and processes
- Workspace state persists through `PersistenceService` + `PersistedRecordReader`/`Writer`; no hand-rolled format, no direct file I/O elsewhere, no legacy persistence symbols. `CheckPersistenceFileIoBoundary`, `CheckNoLegacyPersistenceSymbols`
- Every descriptor-creating call requests close-on-exec on the call itself (`SOCK_CLOEXEC`/`O_CLOEXEC`/`IN_CLOEXEC`); no plain `accept(`. `CheckDescriptorCreationIsCloseOnExec`
- Write/start APIs whose dropped result is a silent failure are `[[nodiscard]]`; a deliberate discard is an explicit `(void)`. Reviewer.
- `FileWriteGate::WriteText` captures a post-write signature only on `Signature::Capture` (the editor's save). Reviewer.
- Git availability is three states (`GitAvailability{Unknown, Repository, NotARepository}`), never a bool. Reviewer.
- Numeric tokens parse through `util/Parse.h`; no `try`/`catch` around `std::sto*`. `CheckThrowingStoParsers`
- LSP `didOpen`/`didChange` are never sent synchronously on the tab-activation path. `CheckLspDidOpenIsNonBlocking`

Plugins
- `lua_State*` and Lua headers stay behind `plugin/LuaRuntime` (sole exemption: `SyntaxDefinitionLoader.cpp`'s sandbox); no `src/plugin/*.cpp` over 800 lines. `CheckLuaStaysBehindPluginBoundary`, `CheckPluginTranslationUnitSize`
- Lua errors never `longjmp` over live C++ destructors: no `luaL_error` in `src/plugin`; raise with `lua_error_util::PushMessage` then `lua_error` after every non-trivial local is gone; rerun `tools/audit-lua-longjmp.py` after touching raise paths. `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals`
- Project reactivation refreshes plugin surfaces, never reloads plugins; reload/shutdown drains async process workers first. `CheckReactivationDoesNotReloadPlugins` (the drain: reviewer)

User-visible text
- Bool settings read with a literal key are declared in `WorkspaceSettingsRegistry`. `CheckSettingsReadAreRegistered`
- Key-hint lists join through `AppendHintSegment`/`JoinHintSegments` (`" · "`). `CheckHintSegmentsUseTheSharedSeparator`
- The two sides of a conflict, merge or compare are **Local** and **Remote** in every user-visible string; "Host" names the remote machine. `CheckSideNamesAreLocalAndRemote`

Durable contracts: `openspec/specs/workspace-architecture/spec.md`,
`persisted-state-format/spec.md`, `shared-edit-primitives/spec.md`; update them in the
same change when an invariant moves.

## Validation Expectations

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before considering work complete.
- Run sanitizer presets for memory/thread-sensitive changes and keep ASAN/UBSAN/TSAN clean.
- Extend and execute relevant fuzz targets when touching persistence and parser pipelines.
- Keep redraw comparison tests serial under SDL dummy video when they share global SDL state.
- Use focused fixtures for git, search, compare, merge, terminal, and plugin-adjacent workflows.
- Run `tools/run-checks.sh qa` after touching damage, redraw, hover, focus or paint code: the redraw verifier (`MICROIDE_QA_VERIFY_REDRAW=1`) compares every partial frame with a full render, so stale paint fails the lane without a reference image. A QA finding lands as a failing-first unit test (`tests/support/RetainedRedraw.h`: `DispatchEvents`, `PaintAndDiffWithFullRepaint`) plus its fix.
- Update docs when a durable architecture decision, workflow, or shipped capability changes.

## Related Docs

- `AGENTS.md`: repo policy, priorities, and iteration loop
- `dev-docs/project/architecture-invariants.md`: each invariant above with the bug that made it a rule
- `dev-docs/project/development-workflow.md`: the full build/test/lane guide, sanitizer and fuzz recipes
- `dev-docs/debugger/dap-integration.md`: debugger/DAP architecture, status, and phased roadmap (shipped on `main` in v2.0.0; Phases 0–10 done)
- `dev-docs/control/control-channel.md`: external control channel for headless/LLM-driven control. Headless entry point: `microide --control [--set <id> <value>]... [--control-spec <file>]` force-starts the channel and mirrors responses/events/applied lines to stdout as JSONL (live AF_UNIX socket + `--control-spec` cold start also supported)
- `dev-docs/project/media-generation.md`: how the showcase screenshots + hero video under `docs/media/` are generated (`tools/capture-media.sh`); regenerate them whenever the UI changes. `tools/release.sh` drives the full release (build → tests → media → signed `.deb`)
- `dev-docs/project/validation-traps.md`: how a green run stops being evidence — stale-binary ctest passes, architecture lints structurally incapable of firing, vacuous fixtures, and the mechanical sweeps (extra warnings, second compiler, hardened stdlib, clone detector, reachability) that found real bugs. Read before trusting a green run or adding a lint rule
- `dev-docs/project/active-work.md`: current shipped baseline and active phases
- `dev-docs/project/implementation-guide.md`: durable product and subsystem map
- `guidelines/architecture.md`: handbook summary of subsystem boundaries
- `guidelines/host-services.md`: service and integration rules
- `guidelines/ui-shell.md`: shell composition and render-path rules
- `guidelines/cpp.md`: C++ ownership and implementation guidance
- `guidelines/plugins.md`: plugin extension rules and boundaries
- `guidelines/performance.md`: profiling and performance expectations
- `guidelines/testing.md`: test strategy and validation loop
