# Agent Guide

Purpose: first-stop operating guide for agents working in this repository.

## Quick Scan

- `microide` is a native desktop IDE shell built in C++20 with CMake and SDL3.
- The current priority is correctness first, then speed, then low CPU usage.
- Plugin support is an active expansion phase; keep plugin seams narrow and host-owned.
- `AGENTS.md` owns repo policy, `dev-docs/project/active-work.md` owns current direction, and `dev-docs/project/implementation-guide.md` owns the durable product map.
- Build with `cmake`, test with `ctest`, and use focused `microide_tests` filters for quick validation.
- Prefer explicit ownership, deterministic helpers, and thin coordinators over broad mutable facades.

## Project Context

`microide` is a compact single-window desktop editor and IDE shell. The codebase currently centers on built-in editor, compare, merge, search, git, terminal, and plugin workflows.

- Language: C++20
- Build system: CMake
- Windowing and input: SDL3
- Optional text backend: SDL3_ttf
- Search engine: PCRE2
- Plugin runtime: Lua 5.4
- Terminal model: PTY-backed terminal sessions
- Test runner: CTest with the in-tree `microide_tests` binary

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

Run the full automated test suite with:

```bash
ctest --test-dir build --output-on-failure
```

Prefer the logging wrapper, which tees all build+test output to a deterministic
file under `/tmp` so results can be read back without rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microide-tests.log
tools/run-checks.sh asan    # -> /tmp/microide-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microide-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microide-tsan.log  (needs vm.mmap_rnd_bits=28)
tools/run-checks.sh all     # all four in sequence
```

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
sudo sysctl vm.mmap_rnd_bits=28
cmake --preset microide-tsan && cmake --build build/microide-tsan -j8 && ctest --test-dir build/microide-tsan --output-on-failure
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
- No `friend class`/`friend struct` in `src/workspace/*`.
- Numeric token parsing uses `util/Parse.h` (`ParseInt`, `ParseInt64`, `ParseSize`, `ParseFloat`). No `try`/`catch` around `std::sto*`.
- The shell stays a thin orchestrator: `src/workspace/WorkspaceShell.h` ≤ 400 lines, `src/workspace/WorkspaceShell.cpp` ≤ 600 lines.
- Workspace-state persistence (project state, user config, session restore) routes through `PersistenceService` plus `PersistedRecordReader`/`PersistedRecordWriter`. Do not hand-roll a new text format or open these files directly elsewhere.
- Single-line input surfaces consume `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`.
- The active editor viewport is owned by the active editor tab; resolve it through `EditorTabService::ActiveViewport()`. Do not reintroduce a shell-level or project-level viewport fallback under any name.
- Plugin host stays decomposed; `lua_State*` lives behind `plugin/LuaRuntime` only, and no `src/plugin/*.cpp` translation unit exceeds 800 lines.
- Plugin Lua errors never `longjmp` over live C++ destructors. The project links the C build of Lua, so `luaL_error` unwinds the native stack without running C++ destructors. Raise via `lua_error_util::PushMessage` (`src/plugin/LuaError.h`) then `lua_error` only after every `std::string`/`std::vector`/`std::filesystem::path` local has destructed; delegating TUs return `lua_error_util::kPendingError` and let the thin `.inc` wrapper raise; wrappers bind null-host fallbacks by reference (no temporaries). `luaL_error` is banned in `src/plugin` by the hard invariant `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` (entry-only `luaL_check*` stays allowed).
- Render TUs covered by the lint (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`, `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`, `WorkspaceShellHoverTargets`, `DebugPaneRender`) consume view models built by `RenderViewModelBuilder` and do not read `context_.current_project_state` or call `CurrentTextInputSurface(...)`. The debug pane's state-reading geometry/scroll helpers live in `DebugPaneLayout.cpp` (not lint-covered), keeping the render TU view-model-only.
- Responsive shell surfaces (`LayoutModeService`, `StatusBarService`, `SettingsOverlayService`) stay host-owned and service-backed. Menu overflow, status-bar actions, Settings, and Help/About route through action/service state and `RenderViewModelBuilder`, not plugin-owned rendering or render-TU product logic.
- No legacy persistence symbols (`WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, `ParseWorkspaceSessionText`) may appear in `src/`, `tests/`, or `tools/`; the legacy importer path is deleted and must not be revived.
- No `platform::RunSubprocess(...)` calls in workspace `.cpp` units; dispatch through `ProjectBackgroundExecutor` to avoid shell-thread stalls.
- Render translation units must not materialize new strings in hot paths (`std::string(...)`, string `+`/`+=`, `to_string`, or `std::format`/`fmt::format`); compute render text in `RenderViewModelBuilder` instead.
- `TextViewport` non-const editing paths must not snapshot-copy `document_->lines`; capture affected ranges only for undo/edit operations.
- Overlay dismissal is centralized: no bare `overlay.visible = false` in `src/workspace/*.cpp` outside `WorkspaceShellOverlay.cpp` (canonical `DismissOverlay`) and `WorkspacePersistenceCoordinatorSession.cpp` (full-state restore reset). Hide overlays via `WorkspaceShell::DismissOverlay` or the focus-safe `HideOverlay(state)` helper so keyboard focus never strands on a hidden surface (enforced by `CheckOverlayDismissalIsCentralized`).

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
- `dev-docs/project/active-work.md`: current shipped baseline and active phases
- `dev-docs/project/implementation-guide.md`: durable product and subsystem map
- `guidelines/architecture.md`: handbook summary of subsystem boundaries
- `guidelines/host-services.md`: service and integration rules
- `guidelines/ui-shell.md`: shell composition and render-path rules
- `guidelines/cpp.md`: C++ ownership and implementation guidance
- `guidelines/plugins.md`: plugin extension rules and boundaries
- `guidelines/performance.md`: profiling and performance expectations
- `guidelines/testing.md`: test strategy and validation loop
