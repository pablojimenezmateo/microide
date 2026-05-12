# Test coverage debt inventory (May 2026)

**Scope:** repository-wide qualitative audit of automated tests, fuzzing, perf harness, and how well they align with `guidelines/testing.md` and `AGENTS.md`.  
**Method:** layout and CMake review, registration sweep in `tests/TestMain.cpp`, `AddTest` / `emplace_back` counts, `ArchitectureInvariantsTests.cpp` rule inventory, targeted ripgrep for SDL dummy video / redraw / FIXME patterns, spot-check of CI fuzz workflow vs. in-tree sources.  
**Not measured:** line, branch, or MC/DC coverage; no `gcov`/`llvm-cov` run in this pass. The primary binary does not expose GoogleTest’s `--gtest_list_tests`; filters are substring- or `--gtest_filter=`-style (see `tests/TestMain.cpp`).

---

## Executive summary

`microide` leans on a **single in-process harness** (`microide_tests`): roughly **~750** registered cases (`AddTest` + `tests.emplace_back` in `tests/*.cpp`), dominated by **workspace shell integration** (`WorkspaceShellProjectTests`, `WorkspaceShellChromeTests`, `WorkspaceShellPluginTests`, `WorkspaceShellSessionTests`, editor/navigation tests, and **terminal**). **Strong:** compare/merge models, project search service, editor essentials, folding, many chrome/layout/redraw contracts, git blame parsing, persisted record I/O and typed state records, plugin host and shell plugin flows, architecture **lint-as-tests** (static rules over `src/`). **Thin or indirect:** dedicated behavioral coverage for **`RenderViewModelBuilder`** as a whole (exercises are scattered in `TextRendererTests`, `EditorEssentialsTests`, allocation tests, status bar), **`ProjectBackgroundExecutor`** and async completion paths (mostly lint + incidental shell tests), **`PersistenceService`** orchestration beyond records/dump, and several **one-test** modules (`FileOperationService`, `RuntimePaths`, `WorkspaceToolDownloader`). **Tooling debt:** `.github/workflows/fuzz.yml` still builds and runs **`LegacyImporterFuzz`**, but the target is **not** defined in root `CMakeLists.txt` and **`tests/fuzz/LegacyImporterFuzz.cpp` is absent**—while `tests/fuzz/corpora/LegacyImporterFuzz/` remains; this is stale contract surface versus the tree. **Flaky / brittle risk:** global **SDL** state and **dummy video** in renderer tests; **no CTest `RUN_SERIAL`/`RESOURCE_LOCK`**, so parallel `ctest -j` across *different* test binaries is safe only if future splits respect shared-state docs; heavy **git/subprocess** and **plugin repo** fixtures increase environmental variance. **Perf:** `microide_perf` + baselines are the behavioral performance oracle; `PerfBaselineTests` only validates baseline **comparison math**, not scenario stability.

---

## Table of contents

1. [Test surface map](#1-test-surface-map)  
2. [Workspace / shell](#2-workspace--shell)  
3. [Editor / text](#3-editor--text)  
4. [Diff / merge](#4-diff--merge)  
5. [Search](#5-search)  
6. [Git](#6-git)  
7. [Terminal](#7-terminal)  
8. [Plugins / Lua](#8-plugins--lua)  
9. [Persistence / records](#9-persistence--records)  
10. [Rendering / view models](#10-rendering--view-models)  
11. [Platform / subprocess / background work](#11-platform--subprocess--background-work)  
12. [Performance harness](#12-performance-harness)  
13. [Architectural lint vs. behavioral coverage](#13-architectural-lint-vs-behavioral-coverage)  
14. [Consolidated gaps and suggested test types](#14-consolidated-gaps-and-suggested-test-types)  
15. [Quick wins appendix](#15-quick-wins-appendix)  
16. [Limitations of this inventory](#16-limitations-of-this-inventory)

---

## 1. Test surface map

### Layout (high level)

| Area | Location | Role |
|------|----------|------|
| Primary unit/integration binary | `microide_tests` (`CMakeLists.txt` lists ~60 `tests/*.cpp` sources) | Custom `TestCase` registry (`tests/TestSupport.h`), not GTest macros |
| Test entry | `tests/TestMain.cpp` | SDL init/shutdown, substring filters, `--gtest_filter=`, `--verbose` |
| Fixtures | `tests/fixtures/`, `tests/perf/fixtures/` | Git, diff, large files, perf scenarios |
| Perf harness | `tests/perf/PerfMain.cpp`, `microide_perf` | Scenario registration, baselines under `tests/perf/baselines/` |
| Fuzz (optional `MICROIDE_FUZZ`) | `tests/fuzz/*.cpp`, corpora under `tests/fuzz/corpora/` | Clang + libFuzzer + ASan |
| Supporting docs | `tests/README.md`, `guidelines/testing.md`, `docs/perf-harness.md` | Expectations, SDL/dummy video note, sanitizer commands |

### CTest registration

- `add_test(NAME microide_tests COMMAND microide_tests)` — **one** default test slot for the full main binary (all cases run sequentially inside the process).
- Optional: `add_test(NAME microide_perf_tests COMMAND microide_perf --smoke --iterations=1 ...)` when `MICROIDE_PERF_HARNESS_BUILD` is on (`cmake --preset microide-perf` per `docs/perf-harness.md`).

No `RUN_SERIAL` or `RESOURCE_LOCK` properties were found under `tests/` or root `CMakeLists.txt`; parallelizing **future** split binaries would need explicit serialization for SDL-heavy suites per `guidelines/testing.md`.

### Filters

- Substrings: `./build/microide/microide_tests TextRenderer` — matches any registered name containing the substring.
- GTest-style: `--gtest_filter=Pattern:-Exclude` (implemented in `TestMain.cpp`).

### Perf and fuzz expectations (from docs / AGENTS)

- Sanitizers: presets `microide-asan`, `microide-ubsan`, `microide-tsan`; TSAN note `vm.mmap_rnd_bits=28` (`guidelines/testing.md`, `AGENTS.md`).
- Fuzz: `MICROIDE_FUZZ=ON`, Clang; **in-tree CMake** currently adds `PersistedRecordReaderFuzz`, `SearchRegexFuzz`, `GitBlameParserFuzz` only.
- **CI fuzz workflow** (`.github/workflows/fuzz.yml`) additionally references **`LegacyImporterFuzz`** — see §16 and §11.

### SDL dummy video / redraw sensitivity

- `EnsureDummySdlVideo()` sets `SDL_VIDEODRIVER=dummy` in `TextRendererTests.cpp`, `EditorRenderViewModelAllocationTests.cpp`, and similar paths.
- Docs: redraw tests sharing global SDL state should stay **serial** under dummy video (`guidelines/testing.md`). Today everything is one binary, so the main risk is **future** test splitting without locks.

---

## 2. Workspace / shell

**Severity — Lower debt (broad behavioral coverage), localized high complexity**

**Evidence:** `WorkspaceShellProjectTests.cpp` (~72 `AddTest` cases), `WorkspaceShellChromeTests.cpp` (~43), `WorkspaceShellSessionTests.cpp` (~36), `WorkspaceShellPluginTests.cpp` (~37), `WorkspaceShellTerminalTests.cpp` (~28), `WorkspaceShellCompareTests.cpp` (~16), `WorkspaceShellPromptTests.cpp` (~16), `WorkspaceShellSearchTests.cpp` (~7), plus shared helpers `WorkspaceShellShared*Tests.cpp`.

**Strong:** command palette, project switch, git sidebar coordination, layout/menus/status, partial redraw contracts (e.g. layout-dirty gating, compare surface skip), plugin-adjacent shell behavior.

**Gaps / risks:**

- **Pixel-perfect / golden redraw tests** are inherently brittle; e.g. comments in chrome tests about isolating aggregate suites—**medium brittle risk** on renderer or font changes.
- **Idle/event-loop** policy (no zero-delay `PollEvent` spin) is enforced by **lint** (`CheckLspDidOpenIsNonBlocking` / related) plus some behavioral tests—not a dedicated “idle hint matrix” suite.
- **Settings / responsive overlays** (`LayoutModeService`, `SettingsOverlayService`) have some registry/layout tests; full **user journey** coverage is uneven.

**Suggested additions:** integration tests for **focus/order edge cases**; explicit **regression** when changing `PrepareFrameOnce` / layout dirty semantics; keep using **view-model assertions** over raw shell state reads in new tests (aligns with architecture rules).

---

## 3. Editor / text

**Severity — Low debt (dense)**

**Evidence:** `EditorEssentialsTests.cpp` (~54 cases), `TextViewportTests.cpp` (~45), `TextRendererTests.cpp` (~33), `EditorSnippetTests.cpp`, `EditorFoldingTests.cpp`, `FoldingModelTests.cpp`, `EditorMultiCaretTests.cpp`, `SingleLineEditorTests.cpp`, `EditorRenderViewModelAllocationTests.cpp`.

**Strong:** bracket scanner, auto-close, surround, smart indent, snippets, folding and viewport motion, multi-caret, whitespace/sticky scroll / occurrence-related builder tests in renderer suite.

**Gaps:** large-file **stress** leans on fixtures and perf scenarios more than unit caps; **IME/composition** paths are harder to see in pure `AddTest` lists—likely thinner.

---

## 4. Diff / merge

**Severity — Low–medium debt**

**Evidence:** `CompareModelTests.cpp` (~14), `MergeModelTests.cpp` (~9), substantial **shell** coverage in `WorkspaceShellCompareTests.cpp` (editable compare, merge labels, scrollbar, blame integration hooks).

**Gaps:** **`microide_diff_bench`** exists as a tool target, not a `ctest` behavioral gate—bench drift is possible. Deep **three-way merge** edge cases may rely on a subset of model tests + shell tests.

**Suggested:** targeted **fixture expansions** for conflict shapes; optional **redraw** assertions for merge gutter/row bands when touching compare render pipeline.

---

## 5. Search

**Severity — Low debt (service), medium (shell integration)**

**Evidence:** `ProjectSearchServiceTests.cpp` (~11), regex utils, `PatternCacheTests.cpp`, `WorkspaceShellSearchTests.cpp` + `WorkspaceShellSharedSearchTests.cpp`.

**Gaps:** replace-in-project and **interactive** search UX may be more shell-heavy—verify when changing replace semantics. Fuzz **`SearchRegexFuzz`** backs regex engine exposure; corpus maintenance under `tests/fuzz/corpora/SearchRegexFuzz`.

---

## 6. Git

**Severity — Medium debt (distribution of tests)**

**Evidence:** `GitServiceTests.cpp`, `GitBlameServiceTests.cpp`, `WorkspaceShellSourceControlTests.cpp`, blame/compare integrations in shell tests; parser fuzz **`GitBlameParserFuzz`**.

**Gaps:** **porcelain edge cases** and **executable bit / rename / conflict** states may be under-exercised outside fixtures. Many tests spawn **real `git`**—environment version differences can cause **low-frequency flakes**.

---

## 7. Terminal

**Severity — Low debt for model; integration breadth varies**

**Evidence:** `TerminalSessionTests.cpp` (~34), `WorkspaceShellTerminalTests.cpp` (~28), `WorkspaceShellSharedTerminalTests.cpp`.

**Strong:** ANSI modes, scroll region, bracketed paste, many shell-level terminal panel behaviors.

**Gaps:** **PTY timing** and **async pump** ordering bugs are historically flaky class—mitigate with deterministic fake clocks only where seams exist; PTY-backed integration remains **medium flake risk**.

---

## 8. Plugins / Lua

**Severity — Medium debt (breadth vs. isolation)**

**Evidence:** `PluginHostTests.cpp` (~15), extensive **`WorkspaceShellPluginTests.cpp`** (save pipeline, sidebars, ESLint/LLM fixture plugins, reload, virtual docs), `Phase3Tests.cpp` (registries: formatters, completions, tools—**uses `assert()` in several cases**, weaker diagnostics than `Expect`).

**Gaps:** **plugin lifecycle** error paths (partial load, bad bytecode, runtime version skew) and **concurrency** with host reload; **Lua C API** coverage is indirect—architecture rule pushes `lua_State` behind `LuaRuntime`; tests should keep validating **host contracts** not internal VM state.

---

## 9. Persistence / records

**Severity — Low debt for format; medium for orchestration**

**Evidence:** `PersistedRecordIoTests.cpp`, `PersistedStateRecordTests.cpp`, `PersistedRecordDumpTests.cpp`, fuzz **`PersistedRecordReaderFuzz`**.

**Strong:** record round-trips, unknown tag skipping, dump paths, reader/writer invariants.

**Gaps:** **`PersistenceService` / session restore** orchestration is less directly named in test file titles—likely covered indirectly via `WorkspaceShellSessionTests` and project tests; worth **explicit** regression tests when touching restore or project reactivation. **`LegacyImporterFuzz`** corpus remains while importer is **removed** from policy—see §16.

---

## 10. Rendering / view models

**Severity — Medium debt (concentration vs. module surface)**

**Evidence:** `TextRendererTests.cpp`, `EditorRenderViewModelAllocationTests.cpp`, chrome/tab render helper tests, architecture rules that **`RenderViewModelBuilder`** must own hot strings and editor VM fields.

**Strong:** occurrence/sticky-scroll caches, allocation regressions, whitespace glyph runs (via essentials + builder).

**Gaps:** No dedicated `RenderViewModelBuilderTests.cpp`; **compare/merge/prompt/sidebar** builder outputs are exercised **through shell or partial units**—risk of **untouched branches** when adding view-model fields. **Hot-path string materialization** is guarded by **lint**, not by runtime proof for every TU.

**Suggested:** small **unit** tests per `Build*` method family when adding new VM payloads; keep **redraw** tests at chrome boundary.

---

## 11. Platform / subprocess / background work

**Severity — Medium behavioral debt; **high** lint coverage**

**Evidence:** `SubprocessTests.cpp`, `TaskExecutorTests.cpp` (2 cases), `BackgroundTaskCounterTests.cpp`, `FileIndexWatcherTests.cpp`, filesystem watcher tests.

**Architectural enforcement:** `CheckNoSynchronousSubprocessWaitInWorkspace`, `CheckNoSynchronousSubprocessInWorkspace`, persistence I/O boundary, LSP non-blocking activation paths.

**Gaps:** **`ProjectBackgroundExecutor`** lacks a named test module—correctness of queueing, cancellation, and project-lifetime pinning is mostly **implicit** in shell/git/search flows. **Failure injection** (slow git, partial reads) is not systematic.

**Tooling gap:** `.github/workflows/fuzz.yml` builds **`LegacyImporterFuzz`**; root `CMakeLists.txt` defines **no** such target—**CI/config drift**.

---

## 12. Performance harness

**Severity — Low for “math”; medium for “scenario completeness”**

**Evidence:** `docs/perf-harness.md` scenario matrix; `tests/perf/baselines/*.json`; `microide_perf --smoke` in optional `CTest`. `PerfBaselineTests.cpp` — **2** tests validating **baseline comparison** tolerance behavior only.

**Gaps:** Scenario **skips** when fixtures are missing (`PerfMain.cpp` messages)—gate scenarios may **silently** not run locally. Long soak / switch scenarios are **expensive**—potential **CI opt-in** only.

**Alignment:** Matches `AGENTS.md` emphasis on `microide_perf_tests` and baseline gates when touching perf-sensitive code.

---

## 13. Architectural lint vs. behavioral coverage

`ArchitectureInvariants/SoftChecks` (and related) implement **~20+ static rules** over sources: no workspace `friend`, coordinator constructor injection, no `std::sto*` throws in scanned areas, plugin TU size, shell file size caps, render TU state access, compare render structural gate, no sync subprocess in workspace, LSP hydration async, no legacy persistence symbols, no full-document `TextViewport` line copies in mutation paths, incremental editor VM vector writes, etc.

**Interpretation:**

| Layer | What it proves | What it does **not** prove |
|-------|----------------|----------------------------|
| Architecture lint | Structural policy, many regressions impossible to merge | Functional correctness, UX, algorithmic output |
| Dense shell tests | End-to-end routing, many user-visible flows | Exhaustive state space; perf |
| Model tests (compare/merge/search) | Deterministic logic | Integration with async services |

**Debt:** Contributors may confuse **green lint** with **behavioral sufficiency**—especially for async/background paths and builder coverage.

---

## 14. Consolidated gaps and suggested test types

**Legend:** U = unit, I = integration, R = redraw/pixel (prefer stable VM asserts over bitmaps), F = fixture-driven.

| Gap | Suggested test type | Notes |
|-----|---------------------|--------|
| `ProjectBackgroundExecutor` queue/cancel/drain | U + I | Deterministic executor fake; shell test for “no stall on git timeout” |
| `PersistenceService` restore / reactivation | I + F | Narrow-record regressions when format or order changes |
| `RenderViewModelBuilder` branch coverage | U | Dedicated TU; assert field presence across compare/merge/prompt |
| IME / composition | I | SDL-driven or seam-level tests if framework allows |
| Git version/env flakes | F + hermetic | Document minimum git; prefer subprocess mocks where feasible |
| Plugin partial failure | I | Corrupt single plugin in multi-plugin project |
| PTY timing | I | Minimize sleeps; inject scripted master writes |
| Perf fixture absence | I (smoke) | Fail or loudly skip in CI only paths—avoid silent skips on gate |
| `LegacyImporterFuzz` / CI | **process** | Remove or restore target; align OpenSpec/workflows/docs |
| Phase3 `assert()` tests | U | Migrate to `Expect` for clearer failures |

---

## 15. Quick wins appendix

1. **Reconcile `LegacyImporterFuzz`:** delete stale workflow steps and corpora, or restore a maintained fuzz target—today’s tree contradicts `.github/workflows/fuzz.yml` and parts of `openspec/specs/bug-detection-tooling/spec.md`.
2. **Name-focused tests for `ProjectBackgroundExecutor`** (even 3–5) with a test double completing jobs on demand.
3. **`RenderViewModelBuilder` smoke** building VMs for compare + merge + sidebar in one test file.
4. Replace **`assert()`** in `Phase3Tests.cpp` with `Expect` for consistent reporting.
5. Document **minimum git** and **SDL dummy** requirements next to failing-test playbooks (`tests/README.md` pointer).
6. When splitting `microide_tests`, add **CTest resource lock** for SDL suites per `guidelines/testing.md`.

---

## 16. Limitations of this inventory

- No **line/branch coverage** or MC/DC; counts are **registration lines**, not unique logical scenarios (some tests exercise multiple assertions).
- **`microide_tests --gtest_list_tests`** was not used; listing is by **registration** inspection (`TestMain.cpp`, `AddTest` grep). A local build could add a small `--list` flag if needed.
- **Production file ↔ test file** mapping is indicative, not exhaustive.
- This document is **point-in-time**; CMake and workflows change—re-audit after fuzz/CI edits.

---

*Inventory prepared as doc-only change for planning; does not alter test behavior or gates.*
