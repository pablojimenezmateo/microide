## Context

After `comprehensive-tech-debt-cleanup` (archived 2026-04-29) the workspace shape, persistence format, and shared single-line model are all locked behind architectural-lint, but the project's performance discipline is still entirely human-driven:

- The sole automated test path runs under `SDL_VIDEODRIVER=dummy`, which short-circuits the renderer; the painted output is never produced, so frame timings, dirty-region behavior, retained-redraw promotion, and per-frame allocation counts are not observable in CI.
- `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, `MICROIDE_TRACE_REDRAW`, `microide_diff_bench`, and `microide_search_bench` exist, but each one is a developer-attached, eyeballed tool. `performance-budgets` even codifies that pattern as the merge gate.
- Multi-project, multi-tab, project-switch, linter-on-save, and idle-soak scenarios are not measured anywhere — neither manually nor automatically.
- The remaining tech-debt items the prior cleanup deferred (oversized coordinator TUs, `WorkspaceShellTestAccess.h`, `util/SingleLineText` vs `editor/SingleLineEditor`, the legacy persistence importer, the documented `WorkspaceLspClient` race) cannot be safely decomposed without a regression oracle, because each split is a service-level move on a hot path.

Constraints from `AGENTS.md`: correctness > speed > low CPU > low memory > maintainability. Compatibility is not a default constraint. From `openspec/specs/performance-budgets/spec.md`: typing, scrolling, idle, and startup budgets are real and must be preserved. From `openspec/specs/workspace-architecture/spec.md`: services own state, coordinators consume narrow service interfaces, render consumes view models. The new harness must respect all of these without forcing a redesign.

Stakeholders: solo maintainer; no external API consumers; plugin authors are dogfood-only.

## Goals / Non-Goals

**Goals:**

- A `microide_perf` binary that drives a real SDL window through scripted scenarios on a software renderer, captures structured metrics, compares against committed baselines, and fails CI on regression beyond a documented per-metric tolerance.
- An initial scenario set that covers cold startup (no project, small project, large project), multi-project switching, multi-tab cycling, typing in small and large files, scrolling in a large file, project search (literal + regex), linter-on-save, compare and merge tab open, chat pane open, and a 30-second idle soak.
- Automated bug-detection coverage: ASAN, UBSAN, and TSAN CI variants for `microide_tests`; libFuzzer harnesses for `PersistedRecordReader`, the legacy importer, the search regex compiler, and the git-blame parser; an allocation-counter fixture that asserts zero allocations during specific render hot paths; an 8-hour nightly idle soak that asserts bounded CPU and RSS.
- A tightened architectural-lint test that covers every render translation unit, hard-fails on plugin and coordinator TU sizes, rejects view-model back-references, and enforces persistence-file-I/O boundaries.
- Deletion of `src/util/SingleLineText.{h,cpp}` and migration of every storage site to `editor::SingleLineEditor`.
- Decomposition of the seven oversized coordinator TUs into focused services or coordinator-shards, each ≤ 800 lines, with the perf harness as the regression oracle.
- Trim of `src/workspace/WorkspaceShellTestAccess.h` to ≤ 600 lines, with affected fixtures rewritten against service-public APIs.
- Resolution of the `WorkspaceLspClient` race noted in the chat-phase-2 memory record, gated on the new TSAN CI variant.
- A scheduled follow-up that removes the legacy persistence importer and `.legacy` files in release +2.

**Non-Goals:**

- Switching the rendering backend, redraw policy, or retained-scene model. The harness measures whatever the host renders.
- Replacing existing diagnostics or telemetry. `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, and `MICROIDE_TRACE_REDRAW` remain as the developer fallback when the harness reports a regression.
- Extending the plugin Lua API surface. Plugins are unchanged.
- Introducing a benchmarking framework (Google Benchmark, Criterion-style harness). The harness is purpose-built and minimal.
- Producing absolute-performance comparisons against user GPU machines. The harness measures relative regression on one fixed software-renderer reference.
- Re-architecting any subsystem outside the listed coordinator decompositions and the SingleLineText collapse.
- Cross-platform runner work. The harness gate runs on Linux only in this change; macOS and Windows perf runs are tracked as a follow-up.

## Decisions

### D1. Software renderer over dummy or GPU

The harness drives the app on `SDL_RENDERER_SOFTWARE` with a fixed window size (1920x1080), DPI 1.0, and the bundled debug font. Software renderer paints to a memory surface, which means the full render path runs (clip rects, retained promotion, per-frame allocations) but the result is deterministic across CI machines.

**Alternative considered:** keep `SDL_VIDEODRIVER=dummy`. Rejected — dummy short-circuits the painter; we cannot observe regressions in the path we care most about.

**Alternative considered:** GPU renderer (OpenGL, Vulkan, Metal). Rejected — varies across CI runners, GPU drivers, and headless display servers; absolute frame times move with the runner; not deterministic enough to gate merges.

**Trade-off:** software-renderer absolute numbers do not match user GPU numbers. The harness gates *relative regression*, not absolute targets. Existing budget thresholds in `performance-budgets` continue to apply as a separate manually-validated check.

### D2. C++ scenarios over a Lua DSL

Scenarios live as C++ files under `tests/perf/scenarios/<name>.cpp`, register themselves via a static registrar, and use a small DSL helper (`Open`, `OpenTab`, `Type`, `Scroll`, `Wait`, `AssertNoAllocations`, etc.) so the common cases stay one line each.

**Alternative considered:** Lua scenario scripts. Rejected — adds the Lua VM as part of the harness runtime, which then becomes a measurement variable; debugging a scenario means debugging a plugin VM rather than stepping through C++.

### D3. Metric set

Each scenario captures, per iteration:

- per-frame `Render` time (percentiles: p50, p90, p95, p99, max)
- full-redraw count vs partial-redraw count
- promote-to-full count
- per-frame allocation count via an instrumented `operator new`/`operator delete` enabled only in `MICROIDE_PERF_HARNESS_BUILD`
- total wall time for the scenario
- CPU time (`getrusage` user + sys)
- RSS at scenario start and end (parse `/proc/self/status`)
- SDL wake-up count (instrumented in the event loop)
- aggregate background-task count (search, blame, LSP request count) as a sanity signal

The reported value per metric is the **median across N=10 iterations** (configurable per scenario). Baselines store every percentile, not just the headline number, so a tail-latency regression cannot hide behind a stable median.

### D4. Baseline storage and CI gate

Baselines live as committed JSON under `tests/perf/baselines/<scenario>.json`, one file per scenario. Each file records the metric set above, the harness build SHA, the OS, the SDL version, and per-metric tolerance windows. Default tolerances: p50 ±10 %, p95 ±20 %, max ±50 %; a scenario may override.

The CI gate runs the harness on a documented reference machine class (CI label `perf-runner-v1`) and exits 1 on regression beyond tolerance. Other CI runners run a smoke-only subset of scenarios that does not gate.

A change author who intends to move a baseline:

1. Re-runs the harness with `--update-baseline <scenario>`.
2. Commits the updated JSON in the same change.
3. Adds a `perf-baseline:` line to the change record explaining the move.

A pre-merge check verifies that any modified `tests/perf/baselines/*.json` file is accompanied by a `perf-baseline:` line in the commit message or change description. This is the entire approval workflow — no out-of-band perf review.

**Alternative considered:** external baseline store (S3, dedicated DB). Rejected — adds infrastructure dependency, slows down review (baselines are no longer visible in the PR diff), and breaks fork-based contributions.

### D5. Determinism rules

- Fixed seed for any randomness in the harness, the app, and any plugin enabled by a scenario.
- Fixed font (the bundled debug font), fixed window size, fixed DPI 1.0.
- Plugins disabled by default in perf runs. A scenario opts in to a specific plugin (e.g., the linter scenario enables one plugin).
- Pre-generated fixture project trees: `tests/perf/fixtures/small_project` (committed, ~50 files), `tests/perf/fixtures/large_project` (committed, ~1000 files), `tests/perf/fixtures/kernel_sized_project` (generated by `tests/perf/generate_kernel_fixture.py` checked into the repo; harness verifies the tree exists and matches a hash before running).
- Frame ticks are scheduled by the scenario itself (`PumpFrames(N)`), not by wall-clock time, so the harness does not measure scheduler jitter.
- The startup-tracer clock and the perf clock are the same `std::chrono::steady_clock` source; the perf harness does not re-mock it. Determinism comes from a fixed scenario script, not from a faked clock — that way the captured numbers reflect real wall time.

### D6. Allocation counter

A small `tests/perf/AllocationCounter.{h,cpp}` overrides global `operator new`/`operator delete` and exposes `Allocations::Snapshot()` for the harness. Compiled in only when `MICROIDE_PERF_HARNESS_BUILD=ON`. Render-path scenarios can assert `AssertNoAllocationsDuringDraw()` to guard against silent regressions like accidental `std::string` construction in a render loop.

**Alternative considered:** sampling-based profiler. Rejected — sampling can miss small allocations that still matter on a hot path. A counter is exact.

### D7. Sanitizer matrix

Three CMake presets (`microide-asan`, `microide-ubsan`, `microide-tsan`) flip `-fsanitize=address|undefined|thread` plus the relevant link flags. CI runs `microide_tests` under each. TSAN is expected to surface the `WorkspaceLspClient` race noted in memory; that fix is part of this change.

**Alternative considered:** Valgrind/Memcheck. Rejected — slow on the SDL path, and ASAN catches the same issues at a fraction of the cost.

### D8. Fuzzing harness

libFuzzer entry points compiled under `MICROIDE_FUZZ=ON`:

- `tests/fuzz/PersistedRecordReaderFuzz.cpp` — feeds arbitrary bytes through `PersistedRecordReader::Decode`.
- `tests/fuzz/LegacyImporterFuzz.cpp` — feeds arbitrary bytes through the one-shot legacy importer.
- `tests/fuzz/SearchRegexFuzz.cpp` — feeds arbitrary patterns through PCRE2 compile + match against a small text fixture.
- `tests/fuzz/GitBlameParserFuzz.cpp` — feeds arbitrary `git blame --porcelain` output through the parser.

Crash corpora live under `tests/fuzz/corpora/<target>/` and are committed. CI runs each fuzzer for a bounded time (60 s default per target on PR; longer in nightly).

The persistence reader and importer get a corresponding requirement in `persisted-state-format/spec.md`: they must survive truncated, swapped-tag, and CRC-corrupt inputs without aborting.

### D9. Architectural-lint upgrades

The `ArchitectureInvariants` test gains:

- A discovery-based render-coverage rule that walks every `src/workspace/WorkspaceShellRender*.cpp` and applies the existing forbidden-pattern checks (no `context_.current_project_state`, no `CurrentTextInputSurface(...)`). The current explicit file list is removed; coverage is automatic from now on.
- A coordinator-size rule (`src/workspace/Workspace*Coordinator*.cpp` ≤ 800 lines, hard-fail).
- A view-model back-reference rule (any type whose name ends in `ViewModel` must not contain a field of type `WorkspaceShell*`, `*Coordinator*`, or `*Service*`; checked by walking declarations of those structs).
- A persistence-file-I/O boundary rule: no file outside `src/workspace/PersistenceService.{h,cpp}`, the legacy importer (until removed), and `src/persistence/*` may open project-state, user-config, session, or conversation files (matched by filename patterns).
- The plugin-TU-size rule (`src/plugin/*.cpp` ≤ 800 lines) is flipped from soft to hard-fail.
- The `try`/`std::sto*` heuristic is replaced with a tokenizing scan that walks `try { ... }` block bodies properly, so multi-line and nested cases are caught.

### D10. SingleLineText collapse

`util::SingleLineTextState` is the older type currently stored in workspace state models (command, prompt, search, overlay, sidebar). `editor::SingleLineEditor` is the canonical model introduced by the prior cleanup. The collapse:

1. Adds a typed adapter `editor::SingleLineEditor::SetText(std::string)`/`Snapshot()` that round-trips state cleanly.
2. Migrates each state model that currently holds `util::SingleLineTextState` to hold `editor::SingleLineEditor` directly. View-model fields are updated in lockstep.
3. Deletes `src/util/SingleLineText.{h,cpp}` and every `util::Set/Get/Insert` free helper.

**Trade-off:** larger blast radius across workspace state files. Acceptable because the prior cleanup already promised this collapse and the harness pins behavior on every step.

### D11. Coordinator decomposition rules

For each oversized coordinator TU (Tab, Action, Chat, Lsp, PersistenceBinaryFormat, Layout, KeyInputSurfaces) we apply the same recipe:

1. Identify the natural seam (e.g., `WorkspaceTabCoordinator` separates lifecycle from save/restore from retarget).
2. Extract a sub-coordinator file or a service in line with the existing service catalog.
3. Update the lint scope so the new file is covered.
4. Run the perf harness for every scenario that touches the affected hot path (e.g., tab cycling, project switch, session restore). Confirm no regression. Update baselines only if intentional.

`WorkspaceShellTestAccess.h` follows the same shape: each method that duplicates a service-public API is replaced with a direct service call in the affected fixture; remaining methods stay only when they expose state genuinely useful for tests and unavailable through services.

### D12. LspClient race fix

The race noted in `project_chat_phase2.md` is treated as an explicit bug investigation step (Phase: Bug fixes). TSAN CI is the oracle. The fix lands behind that gate; if TSAN reports new races during this change's other steps, those are also fixed.

### D13. Sequencing

Order of slices (each independently shippable, each gated by the existing test suite):

1. Foundations: harness skeleton, software-renderer driver, allocation counter, scenario registrar, baseline loader, comparison logic, ctest entry.
2. Initial scenarios + initial baselines.
3. Architectural-lint upgrades.
4. ASAN, UBSAN, TSAN CI variants. LspClient race fix lands here.
5. Fuzzing harnesses + initial corpora.
6. SingleLineText collapse.
7. Coordinator decompositions, one TU per slice, each pinned by the harness.
8. WorkspaceShellTestAccess trim.
9. Long-soak nightly (8 h idle).
10. Schedule the legacy-persistence-cleanup follow-up (release +2).

## Risks / Trade-offs

- [Risk] CI runner noise produces false-positive perf regressions → Mitigation: percentile-based comparison, N=10 iterations per scenario, conservative tolerances on tail metrics, and a single documented reference machine class. The gate runs only on `perf-runner-v1`.
- [Risk] Software-renderer numbers diverge from user GPU experience → Mitigation: harness gates *relative* regression on a fixed renderer; absolute budget thresholds in `performance-budgets` remain as separate manually-validated checks for releases.
- [Risk] Allocation counter overhead skews other metrics → Mitigation: counter is compiled only in the perf-harness build; non-perf and production builds are unaffected.
- [Risk] Scenario set drifts away from real user workloads → Mitigation: every new feature lands with at least one perf scenario covering its hot path; this is a reviewer expectation codified in the modified `performance-budgets` spec.
- [Risk] Coordinator decomposition steps regress behavior because the harness only measures perf, not correctness → Mitigation: each step keeps the existing `microide_tests` suite passing; the harness is additive.
- [Risk] Baselines are noisy on PRs from forks that run on a different runner class → Mitigation: forks see the smoke-only subset and a non-gating advisory output; only main and trunk merges run on `perf-runner-v1`.
- [Risk] Fuzzing finds long-tail bugs that aren't worth fixing immediately → Mitigation: fuzz time is bounded per CI run (60 s on PR, longer nightly); crashes go into a triage queue tracked in `dev-docs/project/known-tech-debt.md`, not as merge blockers, unless they are reachable from real input.
- [Risk] TSAN CI flagging a race the team did not anticipate → Mitigation: TSAN is allowed to soft-fail in the slice that introduces it; once green, it flips to hard-fail. The LspClient race is the known target.
- [Trade-off] Software-renderer-on-headless can require display server setup on CI → Mitigation: SDL software renderer + a virtual framebuffer (`Xvfb` on Linux) is well-trodden; the harness documents the exact `xvfb-run` invocation.
- [Trade-off] Adding 7 coordinator decompositions in one change is broad → Acceptable because each lands as a separate slice gated on the harness; revert is cheap.

## Migration Plan

1. **Land the harness foundations** (skeleton, software-renderer driver, allocation counter, scenario registrar, baseline loader, ctest entry). At this point no scenarios exist, so no gate.
2. **Land initial scenarios + baselines** in one slice. The CI gate becomes active here. Baselines reflect current (pre-cleanup) performance.
3. **Land lint upgrades**, sanitizer matrix, fuzzing harnesses. These are independent of the perf gate.
4. **SingleLineText collapse**: harness pins typing and prompt scenarios; expect zero regression.
5. **Coordinator decompositions**, in any order. Each slice updates the corresponding lint scope and re-runs the harness. Baselines move only when the new structure measurably wins (rare); otherwise the existing baseline holds.
6. **WorkspaceShellTestAccess trim**: pure test refactor; no perf gate change.
7. **LspClient race fix**: gated on TSAN CI.
8. **Long-soak nightly** added as the last slice to give it real weight before release.
9. **Schedule legacy-persistence-cleanup** as a one-line follow-up note in the change record; do not delete `.legacy` files in this change.

Rollback strategy: every slice is a separate commit. The harness itself is additive; reverting a coordinator decomposition is one revert. Baseline movements are visible in the PR diff and reverted with the same commit.

## Open Questions

- **Reference runner class**: dedicated bare-metal runner vs. a fixed VM size on a major cloud provider. Lean: a fixed VM size to avoid the maintenance burden of a bare-metal box; tolerances are conservative enough.
- **Long-soak cadence**: nightly vs. weekly. Lean: nightly, because soak failures are easier to bisect when the window is small.
- **Baseline ownership**: should baselines be regenerated automatically on a maintenance branch, or only via human approval? Lean: human approval only — auto-regeneration would defeat the gate.
- **Plugin-enabled scenarios**: should the linter scenario use the repo's dogfood ESLint plugin, or a fixture plugin that is purpose-built for the harness? Lean: the dogfood plugin, so the scenario reflects real plugin overhead; document the dependency.
- **Allocation counter granularity**: per-thread vs. global. Lean: global for now; revisit if a scenario needs to attribute allocations to background tasks.
- **Chat-composer migration**: how much of the multiline behavior actually justifies a separate model after the SingleLineEditor collapse? Track as an explicit follow-up; do not block this change on it.
