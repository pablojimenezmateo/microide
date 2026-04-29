## 1. Performance Harness Foundations

- [x] 1.1 Add a `MICROIDE_PERF_HARNESS_BUILD` CMake option and a `microide-perf` preset that turns it on alongside the production build flags, plus an `xvfb-run` invocation documented in `docs/perf-harness.md`.
- [x] 1.2 Add `tests/perf/AllocationCounter.{h,cpp}` overriding global `operator new`/`operator delete` behind `MICROIDE_PERF_HARNESS_BUILD`, exposing `Allocations::Snapshot()` and `Allocations::DeltaSince(...)`. Add focused tests for the counter itself.
- [x] 1.3 Add `tests/perf/PerfHarness.{h,cpp}` defining `Scenario`, `ScenarioContext`, the static registrar, `PumpFrames(N)`, `Open(...)`, `OpenTab(...)`, `Type(...)`, `Scroll(...)`, `Wait(...)`, `AssertNoAllocationsDuringDraw()`, and the metric capture (`MetricSet`, `MetricSnapshot`, `Iteration`, `Aggregate`).
- [x] 1.4 Add the SDL software-renderer driver: open a 1920x1080 window with `SDL_RENDERER_SOFTWARE`, force the bundled debug font, force DPI 1.0, disable plugins by default, and parameterize the seeded random source.
- [x] 1.5 Add `tests/perf/PerfMain.cpp` consuming `PerfHarness`, parsing CLI flags (`--scenarios=`, `--update-baseline`, `--smoke`, `--iterations=`, `--report-json=`, `--report-text=`, `--reference-runner=`).
- [x] 1.6 Add baseline loader/comparer in `tests/perf/Baseline.{h,cpp}`: load `tests/perf/baselines/<scenario>.json`, apply per-metric tolerances, compute pass/fail per metric, emit a structured diff. Default tolerances: p50 ±10 %, p95 ±20 %, max ±50 %.
- [x] 1.7 Add a `microide_perf_tests` CTest entry running the smoke subset locally; add a separate CI job entry on `perf-runner-v1` running the full suite as the gate.
- [x] 1.8 Add a pre-merge check that any `tests/perf/baselines/*.json` modification is accompanied by a `perf-baseline:` line in the change record (commit message or PR description). Document the rule in `docs/perf-harness.md`.

## 2. Initial Scenarios And Baselines

- [x] 2.1 Commit fixture project trees: `tests/perf/fixtures/small_project` (~50 files), `tests/perf/fixtures/large_project` (~1000 files). Add `tests/perf/generate_kernel_fixture.py` that produces `kernel_sized_project` deterministically; the harness verifies the tree matches a committed hash before running.
- [x] 2.2 Author scenario `cold_startup_no_project`. Capture metrics, commit baseline.
- [x] 2.3 Author `cold_startup_small_project` and `cold_startup_large_project`. Commit baselines.
- [x] 2.4 Author `multi_project_switch` (open five projects, switch among them N times). Commit baseline.
- [x] 2.5 Author `multi_tab_cycle` (open twenty file tabs, cycle Alt+1..0 patterns). Commit baseline.
- [x] 2.6 Author `typing_small_file` and `typing_large_file`. Commit baselines. Add `AssertNoAllocationsDuringDraw()` to the typing scenarios.
- [x] 2.7 Author `scroll_large_file` covering both wheel and PageDown paths. Commit baseline.
- [x] 2.8 Author `project_search_literal` and `project_search_regex` driven against `kernel_sized_project`. Commit baselines.
- [x] 2.9 Author `linter_on_save` enabling the dogfood ESLint plugin and asserting save-to-diagnostics-published wall time. Commit baseline.
- [x] 2.10 Author `compare_tab_open` and `merge_tab_open` against committed compare/merge fixtures. Commit baselines.
- [x] 2.11 Author `chat_pane_long_transcript` opening the chat pane and pasting a long fixture transcript. Commit baseline.
- [x] 2.12 Author `idle_soak_30s` covering 30 s of post-startup idle, asserting per-second wake-up budget. Commit baseline.
- [ ] 2.13 Run the harness in `--update-baseline` mode on `perf-runner-v1` to seed every committed baseline. Flip the gate to active.

## 3. Architectural-Lint Upgrades

- [x] 3.1 In `tests/ArchitectureInvariantsTests.cpp`, replace the explicit render-file list in `CheckRenderSurfaceStateAccess` with a discovery walk over `src/workspace/WorkspaceShellRender*.cpp`. Add coverage for the previously-uncovered `WorkspaceShellRenderChrome.cpp`, `WorkspaceShellRenderMenus.cpp`, `WorkspaceShellRenderPrompts.cpp`, and `WorkspaceShellRender.cpp`.
- [x] 3.2 Add `CheckCoordinatorTuSize` rule (`src/workspace/Workspace*Coordinator*.cpp` ≤ 800 lines, hard-fail). Mark soft-fail until step 8 finishes, then flip.
- [x] 3.3 Add `CheckViewModelBackReferences` rule walking declarations of types whose name ends in `ViewModel` and rejecting fields of type `WorkspaceShell*/&`, `*Coordinator*`, or `*Service*`. Hard-fail.
- [x] 3.4 Add `CheckPersistenceFileIoBoundary` rule rejecting `std::ifstream`/`std::ofstream`/`std::fopen`/`open(` on filenames matching the documented project-state, user-config, session, and conversation patterns outside `PersistenceService.{h,cpp}`, the legacy importer, and `src/persistence/*`. Hard-fail.
- [x] 3.5 Flip `CheckPluginTranslationUnitSize` to hard-fail. Confirm no current violations.
- [x] 3.6 Replace the regex-based `try`/`std::sto*` heuristic in `CheckThrowingStoParsers` with a tokenizing scan that walks `try { ... }` block bodies (track brace depth, ignore strings and comments). Add focused tests against contrived multi-line and nested examples.
- [x] 3.7 Run `ctest --test-dir build/microide --output-on-failure` and confirm the lint test passes.

## 4. Sanitizer CI Variants

- [x] 4.1 Add CMake presets `microide-asan`, `microide-ubsan`, `microide-tsan` with the appropriate `-fsanitize=` flags and link options.
- [x] 4.2 Add three CI matrix entries running `microide_tests` under each preset. ASAN and UBSAN gate immediately; TSAN starts as soft-fail.
- [x] 4.3 Investigate and fix the `WorkspaceLspClient` race noted in the chat-phase-2 memory record. Add a focused regression test and confirm TSAN reports clean.
- [x] 4.4 Triage any further races, leaks, or undefined-behavior reports surfaced by the new CI variants. Fix or file in `docs/known-tech-debt.md` per the bug-detection-tooling spec.
- [x] 4.5 Flip TSAN from soft-fail to hard-fail.

## 5. Fuzzing Harnesses

- [x] 5.1 Add `MICROIDE_FUZZ` CMake option and the corresponding compile/link flags for libFuzzer.
- [x] 5.2 Add `tests/fuzz/PersistedRecordReaderFuzz.cpp` calling `PersistedRecordReader::Decode` on libFuzzer input. Seed corpus from existing valid persistence files plus a few hand-crafted edge cases.
- [x] 5.3 Add `tests/fuzz/LegacyImporterFuzz.cpp` driving the legacy importer end-to-end on libFuzzer input.
- [x] 5.4 Add `tests/fuzz/SearchRegexFuzz.cpp` feeding arbitrary patterns through the PCRE2 compile + match path.
- [x] 5.5 Add `tests/fuzz/GitBlameParserFuzz.cpp` feeding arbitrary `git blame --porcelain` output through the parser.
- [x] 5.6 Commit initial corpora under `tests/fuzz/corpora/<target>/`.
- [x] 5.7 Add a CI job running each fuzzer for 60 s on PR; add a nightly job running each for an extended documented duration.
- [x] 5.8 Triage fuzz findings. Fix issues that are reachable from real input or indicate memory unsafety; file the rest in `docs/known-tech-debt.md`.

## 6. SingleLineText Collapse

- [ ] 6.1 Add the migration adapters on `editor::SingleLineEditor` (`SetText(std::string)`, `Snapshot()`, `Append(std::string)`) needed to round-trip the legacy state cleanly.
- [ ] 6.2 Migrate every workspace state model that currently holds `util::SingleLineTextState` to hold `editor::SingleLineEditor` directly. Update view models in lockstep.
- [ ] 6.3 Migrate every render call site that reads `const util::SingleLineTextState*` to take `const editor::SingleLineEditor*`. Update `RenderViewModelBuilder` accordingly.
- [ ] 6.4 Delete `src/util/SingleLineText.h`, `src/util/SingleLineText.cpp`, and every `util::Set/Get/Insert*` free helper. Confirm zero references via `grep`.
- [ ] 6.5 Run the harness scenarios that exercise the migrated surfaces (`typing_small_file`, `typing_large_file`, `project_search_literal`, `chat_pane_long_transcript`, `multi_tab_cycle`). Confirm green; only update baselines if intentional.

## 7. Chat Composer Equivalent-Behavior Reuse

- [ ] 7.1 Identify chat-composer event paths that are behaviorally single-line (selection-range, clipboard, select-all on the visible line).
- [ ] 7.2 Route those paths through `SingleLineKeyHandler` and the shared model; keep the multiline storage for newline/vertical-movement/page-nav.
- [ ] 7.3 Update `docs/text-surface-unification.md` to describe the shrunk multiline exception. Add a regression test asserting the shared paths route through the canonical model.
- [ ] 7.4 Re-run `chat_pane_long_transcript`. Confirm green.

## 8. Coordinator-TU Decomposition

- [ ] 8.1 Decompose `WorkspaceTabCoordinator.cpp` (1364 lines) into focused units (e.g. lifecycle, save/restore, retarget). Each ≤ 800 lines. Re-run harness `multi_tab_cycle`, `cold_startup_*`. Update lint scope.
- [ ] 8.2 Decompose `WorkspaceActionServices.cpp` (1086 lines) along the existing per-domain executor seams. Re-run relevant scenarios (typing, project switch, sidebar). Update lint scope.
- [ ] 8.3 Decompose `WorkspaceChatTranscript.cpp` (1170 lines) into rendering, persistence, and tool-event handling units. Re-run `chat_pane_long_transcript`. Update lint scope.
- [ ] 8.4 Decompose `WorkspaceLspClient.cpp` (1126 lines) along request-type seams. Re-run scenarios that touch LSP (`linter_on_save` and any added LSP-driven scenario). Update lint scope.
- [ ] 8.5 Decompose `WorkspacePersistenceBinaryFormat.cpp` (1119 lines) by record type. Re-run `cold_startup_*`. Update lint scope.
- [ ] 8.6 Decompose `WorkspaceLayout.cpp` (995 lines) into per-pane geometry helpers. Re-run scroll and resize scenarios. Update lint scope.
- [ ] 8.7 Decompose `WorkspaceKeyInputCoordinatorSurfaces.cpp` (735 lines, soft-cap) along surface-domain seams. Re-run typing scenarios. Update lint scope.
- [ ] 8.8 Flip the coordinator-TU-size lint rule from soft-fail to hard-fail once every unit is ≤ 800 lines.

## 9. WorkspaceShellTestAccess Trim

- [ ] 9.1 Audit `src/workspace/WorkspaceShellTestAccess.h` (~1500 lines). Categorize each method as: (a) duplicates a service-public API, (b) exposes test-only state genuinely unavailable through services, (c) obsolete.
- [ ] 9.2 Replace category-(a) call sites in fixtures with direct service calls; delete the methods.
- [ ] 9.3 Remove category-(c) methods.
- [ ] 9.4 Cap the remaining file at ≤ 600 lines. Add a lint rule for the cap.
- [ ] 9.5 Run the full `microide_tests` suite. Confirm green.

## 10. Bug Triage And Documentation

- [ ] 10.1 Triage every non-blocking finding from sanitizer CI, fuzz CI, and the long-soak. Add reproduction steps and severity to `docs/known-tech-debt.md`.
- [x] 10.2 Update `docs/runtime-profiling.md` and `docs/startup-tracing.md` to describe the harness as the primary regression oracle and the env-traces as the developer fallback.
- [x] 10.3 Add `docs/perf-harness.md` covering: how to add a scenario, how to update a baseline, the `perf-baseline:` change-record convention, the reference machine class, and the smoke vs. gate split.
- [x] 10.4 Update `guidelines/performance.md`, `guidelines/architecture.md`, `guidelines/testing.md`, `AGENTS.md`, and `CLAUDE.md` to reference the harness, the sanitizer matrix, the fuzzing harnesses, and the new lint rules.
- [x] 10.5 Update `docs/active-work.md` to reflect the harness, sanitizer matrix, fuzzing, and coordinator-decomposition completions as the new shipped baseline.
- [ ] 10.6 Close items in `docs/known-tech-debt.md` that this change resolves (`WorkspaceShellTestAccess` trim, `util/SingleLineText` collapse, oversized coordinator TUs, lint coverage gaps, LspClient race).

## 11. Long-Soak Nightly

- [ ] 11.1 Author scenario `long_soak_8h` reusing the idle harness driver, parsing RSS at start/midpoint/end and CPU continuously, asserting per-hour wake-up budget.
- [ ] 11.2 Add a nightly CI job invoking only `long_soak_8h`. Configure thresholds.
- [ ] 11.3 Run the nightly once manually before declaring it the gate; resolve any leaks or wake-ups it surfaces.

## 12. Scheduled Follow-Up: legacy-persistence-cleanup

- [x] 12.1 Add `follow-ups.md` to this change recording the `legacy-persistence-cleanup` follow-up: target release, files to delete (`WorkspacePersistenceLegacyFormat.{h,cpp}`, importer call sites, `<file>.legacy` files), and the harness scenarios that must be green at the time of the cleanup. Do not delete the legacy importer in this change.

## 13. Final Validation

- [ ] 13.1 Run `cmake --build build/microide` clean; run `ctest --test-dir build/microide --output-on-failure`; resolve any flake.
- [ ] 13.2 Run `microide_perf_tests` on `perf-runner-v1`. Confirm every baseline is green.
- [ ] 13.3 Run all sanitizer CI variants and fuzz CI on the final state. Confirm green.
- [ ] 13.4 Run `openspec status --change "comprehensive-tech-debt-and-perf-harness"` and confirm all artifacts are done; archive the change.
