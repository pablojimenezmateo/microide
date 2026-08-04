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
| `lsp/`, `debug/`, `git/`, `persistence/`, `control/` | subsystem-owned clients, protocols, models, and presentation |
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

## Development Workflow

Configure and build with the repo's CMake flow:

```bash
cmake -S . -B build
cmake --build build -j8
```

For the inner test loop, build only the test binary — `ctest` invokes just
`microide_tests`, so this skips the production `microide` executable and the
bench binaries (roughly halves the build):

```bash
cmake --build build --target microide_tests -j8
```

The build auto-uses **ccache** (compiler cache) and **ld.lld** (fast linker)
when installed; both are no-ops if absent. For the best inner-loop speed install
`ccache` once (`sudo apt install ccache`, then `ccache --max-size=10G`); the
shared `microide_core` object library means core compiles once across the
`microide`/`microide_tests`/`microide_perf` targets. `tools/run-checks.sh`
already exports the `CCACHE_SLOPPINESS` needed for ccache to cache PCH builds.

Run the full automated test suite with:

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```

The suite is registered as `MICROIDE_TEST_SHARDS` ctest tests (default 24), each
a `microide_tests --shard-index=I --shard-count=N` process running a
deterministic round-robin slice, so `ctest -jN` uses every core instead of
running the whole suite in one serial process. This is the dominant win for the
sanitizers, which used to run 18–30 min single-threaded. `run-checks.sh` passes
`-j` automatically (full width for plain runs, capped at 6 for the
memory-heavy sanitizers; override with `MICROIDE_CTEST_JOBS` /
`MICROIDE_CTEST_SAN_JOBS`). Set `-DMICROIDE_TEST_SHARDS=1` to get the old single
`microide_tests` test back. Run one shard directly with
`./build/microide/microide_tests --shard-index=0 --shard-count=24`.

Prefer the logging wrapper, which tees all build+test output to a deterministic
file under `/tmp` so results can be read back without rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microide-tests.log
tools/run-checks.sh asan    # -> /tmp/microide-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microide-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microide-tsan.log  (no sudo needed; see below)
tools/run-checks.sh perf-tests  # -> /tmp/microide-perf-tests.log (allocation counting armed)
tools/run-checks.sh all     # all four in sequence
```

The default `tests` target leaves `MICROIDE_PERF_HARNESS_BUILD` OFF, which is what
arms the counting `operator new`/`delete`. Every `#if MICROIDE_PERF_HARNESS_BUILD`
assertion — all of `PluginPresentationAllocationTests` and
`EditorRenderViewModelAllocationTests`, plus parts of `TextViewportTests` — therefore
compiles to nothing there. Run `perf-tests` after touching a render, view-model, or
presentation hot path, or those "must not allocate" contracts go unchecked.

After a run, READ `/tmp/microide-<target>.log` instead of rebuilding and rerunning.

Run focused tests with one or more substring filters:

```bash
./build/microide/microide_tests TextRenderer
./build/microide/microide_tests "WorkspaceShell/EditorDirty"
```

When a change affects performance-sensitive code, run relevant `dev-docs/performance/perf-harness.md` scenarios first, then use startup/runtime tracing docs for deeper diagnosis.

Sanitizer and fuzz workflows expected for risky changes:

```bash
cmake --preset microide-asan && cmake --build build/microide-asan -j8 && ctest --test-dir build/microide-asan --output-on-failure
cmake --preset microide-ubsan && cmake --build build/microide-ubsan -j8 && ctest --test-dir build/microide-ubsan --output-on-failure
cmake --preset microide-tsan && cmake --build build/microide-tsan -j8
# No sudo: setarch -R clears ASLR for this process only. `sudo sysctl
# vm.mmap_rnd_bits=28` is the machine-wide fallback if personality() is blocked.
setarch -R env TSAN_OPTIONS=suppressions=tests/tsan.supp \
  ctest --test-dir build/microide-tsan --output-on-failure
cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/microide-fuzz -j8
./build/microide-fuzz/microide/PersistedRecordReaderFuzz -max_total_time=60 tests/fuzz/corpora/PersistedRecordReaderFuzz
```

## Architecture Stance

- Correctness beats compatibility unless compatibility is explicitly required.
- Plugin expansion should tighten host boundaries, not widen ad hoc shell access.
- Rendering, redraw policy, layout, and shell chrome remain host-owned.
- External tools and OS integration should stay behind narrow services in `src/project/*`, `src/platform/*`, or similarly focused modules.
- Coordinators should translate input or intent into state changes and service calls; they should not become long-lived ownership sinks.
- If a subsystem boundary is wrong, fix the boundary rather than adding a compatibility shim around it.

## Hard Architectural Invariants

Several patterns were intentionally removed by the 2026-04-29 `comprehensive-tech-debt-cleanup` change. Do not reintroduce them. The architectural-lint test (`tests/ArchitectureInvariantsTests.cpp`) hard-fails on the load-bearing ones; the policy ones are reviewer-enforced.

- Workspace coordinator constructors take service-interface references, never `WorkspaceShell&` or `WorkspaceShell*`. Services live alongside the shell (`EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `PersistenceService`).
- No `friend class`/`friend struct` in `src/workspace/*`, except the sanctioned `WorkspaceShell::TestAccess` backdoor (now declared unconditionally so the shared `microide_core` object library compiles an ODR-identical `WorkspaceShell` for the production and test binaries; the lint exempts friends naming a `*TestAccess` type).
- Numeric token parsing uses `util/Parse.h` (`ParseInt`, `ParseInt64`, `ParseSize`, `ParseFloat`). No `try`/`catch` around `std::sto*`.
- The shell stays a thin orchestrator: `src/workspace/shell/WorkspaceShell.h` ≤ 400 lines, `src/workspace/shell/WorkspaceShell.cpp` ≤ 600 lines.
- Workspace-state persistence (project state, user config, session restore) routes through `PersistenceService` plus `PersistedRecordReader`/`PersistedRecordWriter`. Do not hand-roll a new text format or open these files directly elsewhere.
- Single-line input surfaces consume `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`.
- The active editor viewport is owned by the active editor tab; resolve it through `EditorTabService::ActiveViewport()`. Do not reintroduce a shell-level or project-level viewport fallback under any name.
- Plugin host stays decomposed; `lua_State*` lives behind `plugin/LuaRuntime` only, and no `src/plugin/*.cpp` translation unit exceeds 800 lines. Production code outside `src/plugin` must not name `lua_State`, include a Lua header, or include a Lua-exposing plugin header (transitively) — enforced by the hard invariant `CheckLuaStaysBehindPluginBoundary`; the outward-facing surfaces (`PluginHost.h`, `PluginThread.h`) stay Lua-free at the type level. Sole exemption: `src/editor/SyntaxDefinitionLoader.cpp`'s self-contained sandbox state.
- Plugin Lua errors never `longjmp` over live C++ destructors. The project links the C build of Lua, so `luaL_error` unwinds the native stack without running C++ destructors. Raise via `lua_error_util::PushMessage` (`src/plugin/LuaError.h`) then `lua_error` only after every `std::string`/`std::vector`/`std::filesystem::path` local has destructed; delegating TUs return `lua_error_util::kPendingError` and let the thin `.inc` wrapper raise; wrappers bind null-host fallbacks by reference (no temporaries). `luaL_error` is banned in `src/plugin` by the hard invariant `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` (entry-only `luaL_check*` stays allowed). After touching raise paths in `src/plugin`, rerun the AST scope audit `tools/audit-lua-longjmp.py` (verifies no raise-capable Lua call has a live non-trivially-destructible local in scope; see its docstring for setup).
- Render TUs covered by the lint (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`, `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`, `WorkspaceShellHoverTargets`, `DebugPaneRender`) consume view models built by `RenderViewModelBuilder` and do not read `context_.current_project_state` or call `CurrentTextInputSurface(...)`. The debug pane's state-reading geometry/scroll helpers live in `DebugPaneLayout.cpp` (not lint-covered), keeping the render TU view-model-only.
- Render view models own their data (`CheckRenderViewModelsOwnProjectState`, TD-2026-07-17-084/26): no `OverlayState` in `RenderViewModelBuilder.h`, and a `ProjectWorkspaceState*` only in the two documented escape hatches (`FrameSurfaceViewModel`, `SidebarSurfaceViewModel`). The converted render TUs (Overlay, BottomPanel, TextInput, HoverPopup, HoverTargets, DebugPaneRender) must not name either broad state type — labels/rows are precomposed in the builder (owned `label_storage` blob or frame-stable views), and paint-time state mutation moved to frame prep (overlay scroll clamp, bottom-panel tab-strip fill, `PrepareCommitBodyViewportForFrame`).
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay host-owned and service-backed. Menu overflow, status-bar actions, Settings, and Help/About route through action/service state and `RenderViewModelBuilder`, not plugin-owned rendering or render-TU product logic.
- No legacy persistence symbols (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, `ParseWorkspaceSessionText`) may appear in `src/`, `tests/`, or `tools/`; the legacy importer path is deleted and must not be revived.
- No `platform::RunSubprocess(...)` calls in workspace `.cpp` units; dispatch through `ProjectBackgroundExecutor` to avoid shell-thread stalls.
- Render translation units must not materialize new strings in hot paths (`std::string(...)`, string `+`/`+=`, `to_string`, or `std::format`/`fmt::format`); compute render text in `RenderViewModelBuilder` instead. The compare/merge surface render TUs are named `WorkspaceShellRenderCompare.cpp` / `WorkspaceShellRenderMerge.cpp` (the `WorkspaceShellRender*` prefix is what pulls them under the lint set); do not rename them back to `*CompareRender`/`*MergeRender` or they silently escape the string, single-char, and structural-gate rules. They render one `editor::DecoratedTextRow` per visible row via reused scratch members (`compare_left/right_scratch_row_`, `merge_incoming/current_scratch_row_`) — never a fresh per-row `DecoratedTextRow` — and truncate hot labels through `TruncateLabelView` (allocation-free fits-case), enforced by `CheckCompareMergeRenderUsesScratchRows` and `CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings`.
- `TextViewport` non-const editing paths must not snapshot-copy `document_->lines`; capture affected ranges only for undo/edit operations.
- Overlay dismissal is centralized: no bare `overlay.visible = false` in `src/workspace/*.cpp` outside `WorkspaceShellOverlay.cpp` (canonical `DismissOverlay`) and `WorkspacePersistenceCoordinatorSession.cpp` (full-state restore reset). Hide overlays via `WorkspaceShell::DismissOverlay` or the focus-safe `HideOverlay(state)` helper so keyboard focus never strands on a hidden surface (enforced by `CheckOverlayDismissalIsCentralized`).
- Every descriptor-creating call (`socket`, `accept4`, `open`/`openat`, `pipe2`, `inotify_init1`) must request close-on-exec **on the creating call itself** (`SOCK_CLOEXEC` / `O_CLOEXEC` / `IN_CLOEXEC`), never via a follow-up `fcntl(F_SETFD)` — another thread can fork/exec in the window between the two. The editor spawns terminal shells, LSP servers, DAP adapters, git, and plugin tools, so an unflagged descriptor is inherited by all of them; for the control socket that is a containment hole, not just a leak. Enforced by `CheckDescriptorCreationIsCloseOnExec`, which also bans plain `accept(`, code-masks matches (so `open(`/`socket(` inside string literals are not call sites), and fails loudly if it finds no call sites at all rather than passing vacuously. That rule shipped with `openat?` as its `open` pattern — which matches `opena`/`openat` and never plain `open(` — so it passed green while blind; it now carries negative + positive control fixtures in `tests/architecture/ArchitectureRuleFixtures.cpp`, and any new lint should too.
- Every bool setting read in `src/` through `SettingFlagEnabled`/`SettingEnabled`/`SettingOn`/`SettingBoolIsOn` with a literal dotted key must be declared in `WorkspaceSettingsRegistry`. An unregistered key is silent — it reads as its default forever, stays out of the Settings overlay, and is reachable only by hand-editing a config file with a name found in a source comment. Enforced by `CheckSettingsReadAreRegistered` (code-masked, with the same loud-missing-target guard).

- Key-hint lists join through `AppendHintSegment`/`JoinHintSegments` in `workspace/WorkspaceUiText.h` (separator `kHintSeparator`, `" · "`). Do not re-declare `AppendHintSegment` in a TU — two files independently grew byte-identical private copies joining on `"  |  "`, which is how the git sidebar's action line and the compare review header ended up spelled differently from every other hint in the app (both are also mirrored into Help/About, where the two spellings sat three rows apart). Enforced by `CheckHintSegmentsUseTheSharedSeparator`, which targets the redefinition rather than the `"  |  "` literal — that separator is legitimate between unrelated fields (the breadcrumb's `path | left -> right`, the merge status line).

- Buffer-content filetype detection goes through `TextViewport::language_id()`, which memoizes on (document identity, content revision, path, syntax-registry revision). Four independent caches for this had accreted — `runtime_syntax::FiletypeMemo` with two instances, plus `LspUiState::language_cache_*`, whose path-only key silently pinned a stale language for content-detected buffers (a shebang with no extension). The content-reading two-argument `DetectFiletype(path, lines)` overload is reserved for that memo; the one-argument path-only form stays legal for callers with no buffer in hand. Enforced by `CheckViewportFiletypeGoesThroughTheViewportMemo` (code-masked, arity-aware, loud on a missing target).

- Same-language editor tabs share ONE `editor::LanguageContractView`, held by `shared_ptr<const>` and built/cached by `WorkspaceLanguageContract::ResolveEditorView` (keyed by language id, dropped whole when the contract revision or any of the three editor toggles changes). Do not give `TextViewport` an owned view back: `ApplyEditorPreferences` walks every open tab, so an owned view means one deep copy of four vectors and three strings per tab per settings change, project activation and session restore. Gated by the `settings_change_many_tabs` perf scenario (TD-2026-08-03-110).

The durable contracts live in `openspec/specs/workspace-architecture/spec.md`, `openspec/specs/persisted-state-format/spec.md`, and `openspec/specs/shared-edit-primitives/spec.md`. The full reasoning is in `AGENTS.md` § Do-Not-Regress Patterns and in the archived change at `openspec/changes/archive/2026-04-29-comprehensive-tech-debt-cleanup/`.

## Validation Expectations

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before considering work complete.
- Run sanitizer presets for memory/thread-sensitive changes and keep ASAN/UBSAN/TSAN clean.
- Extend and execute relevant fuzz targets when touching persistence and parser pipelines.
- Keep redraw comparison tests serial under SDL dummy video when they share global SDL state.
- Use focused fixtures for git, search, compare, merge, terminal, and plugin-adjacent workflows.
- Update docs when a durable architecture decision, workflow, or shipped capability changes.

## Related Docs

- `AGENTS.md`: repo policy, priorities, and iteration loop
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
