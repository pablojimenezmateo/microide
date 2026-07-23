# Testing Guide

Purpose: define the durable testing strategy and minimum validation expectations for `microide`.

## Quick Scan

- Test behavior and ownership boundaries, not incidental implementation details.
- Every meaningful bug fix should add or tighten regression coverage.
- Use the existing CMake and CTest flow; the primary automated binary is `microide_tests`.
- Prefer focused subsystem tests and fixtures over broad end-to-end-style harnesses.
- Redraw-sensitive tests that share SDL global state should stay serial under SDL dummy video.

## Test Stack

- Build and register tests through CMake.
- Run the full suite with `ctest --test-dir build --output-on-failure`.
- Run focused coverage with `./build/microide/microide_tests <filter>`.
- Keep fixtures and test support under `tests/`.

Useful commands:

```bash
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/microide/microide_tests TextViewport
```

Logging wrapper (tees build+test output to `/tmp/microide-<target>.log` so results
can be read back without rerunning — prefer this for full suite and sanitizer runs):

```bash
tools/run-checks.sh tests   # -> /tmp/microide-tests.log
tools/run-checks.sh asan    # -> /tmp/microide-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microide-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microide-tsan.log
tools/run-checks.sh all
```

After a run, read `/tmp/microide-<target>.log` instead of rebuilding and rerunning.

Sanitizer and fuzzing commands:

```bash
cmake --preset microide-asan && cmake --build build/microide-asan -j8 && ctest --test-dir build/microide-asan --output-on-failure
cmake --preset microide-ubsan && cmake --build build/microide-ubsan -j8 && ctest --test-dir build/microide-ubsan --output-on-failure
sudo sysctl vm.mmap_rnd_bits=28
cmake --preset microide-tsan && cmake --build build/microide-tsan -j8 && ctest --test-dir build/microide-tsan --output-on-failure
cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/microide-fuzz -j8
./build/microide-fuzz/microide/PersistedRecordReaderFuzz -max_total_time=60 tests/fuzz/corpora/PersistedRecordReaderFuzz
```

## Coverage Expectations

Add or update tests when work changes:

- editor model behavior
- compare or merge behavior
- project search, indexing, or file operations
- git integration and parser behavior
- terminal session behavior
- plugin lifecycle, registries, or plugin-facing contracts
- redraw invalidation or shell interaction behavior

Prefer the smallest harness that proves the behavior while still exercising the real boundary that is at risk.

## Fixture Strategy

- Use focused committed fixtures for git, search, compare, merge, syntax, and large-file scenarios.
- Keep fixtures explicit and scenario-specific.
- Extend `tests/fixtures/` or `tests/TestSupport.*` when the same setup will be reused.
- Regenerate fixture corpora through documented scripts such as `tests/generate_fixtures.py` instead of hand-editing generated manifests.

## Cross-Thread And Timing

Tests that exercise background-thread work (file watchers, PTY/terminal, subprocess,
DAP/LSP clients, `ProjectBackgroundExecutor`/`TaskExecutor`, project search, the control
socket) must not assert state immediately after a fixed `std::this_thread::sleep_for`.
A single check after a fixed wait races the background thread and goes intermittently
red on otherwise-correct code.

- **Poll a condition until a bounded deadline.** Repeatedly drain/check inside a
  `while (steady_clock::now() < deadline)` loop with a short sleep between iterations.
  Reuse the existing helpers (`WaitFor`, `PollUntil`, `WaitForProjectReload`,
  `WaitForLspReadinessState`, `ExchangeLine`) or follow their shape.
- **Prefer a deterministic signal over any sleep** where one exists — a drained
  callback, a future, a `condition_variable`, or an `std::atomic` flag the background
  thread sets (e.g. the `reader_running` signal in `tests/SubprocessTests.cpp`).
- **Absence assertions** ("must NOT fire / must NOT publish after X") still need a
  bounded quiet window: drain continuously for that window and assert nothing valid ever
  appears, rather than checking once.
- A fixed `sleep_for` is only acceptable for a genuine **timing-bound** assertion
  ("operation returns within N ms") or to defeat coarse filesystem mtime granularity
  before a synchronous sample — never to wait for a background event to land.
- **Watcher backends share one contract suite** (`tests/FileIndexWatcherContractTests.cpp`,
  TD-2026-07-17-036): behavioral changes to `FileIndexWatcher` belong there, asserted
  through the `project::FileIndex` end state so they hold for every backend. Force the
  poll fallback with `SetForcePollForTesting(true)` + a short `SetPollIntervalForTesting`;
  a new platform backend must pass the suite unchanged.
- **Terminal teardown paths are stress-covered with real PTYs**
  (`tests/TerminalLifecycleStressTests.cpp`, TD-2026-07-17-015). The rest of the suite
  runs placeholder terminals; if you touch backend/session shutdown (Stop, the signal
  ladder, reader-thread lifetime), extend that suite — and remember its assertions only
  bite fully under the sanitizers.

## UI And SDL Caveats

- Tests that initialize SDL should be deterministic under the dummy video driver.
- Redraw comparison tests that share global SDL state should not be parallelized.
- Keep shell tests focused on observable state, action routing, dirty-region behavior, and rendered contracts that the host intends to preserve.
- `WorkspaceShell/ShiftAltClickAddsColumnCarets` exercises Shift+Alt column caret placement in the active editor viewport. It disables sticky-scroll metrics adjustment, resets modifier state, and renders one frame before reading editor metrics so column-hit routing matches `EditorMouseCoordinator`. Avoid local names like `has_line0` / `has_line1` in tests that include SDL headers; X11 macros can shadow them and make boolean checks lie even when caret state is correct.
- `WorkspaceLspClient/DidOpenQueuedBeforeInitializeStillDeliversFullText` starts a Python LSP stub with a delayed `initialize` response. The test polls the marker file for up to five seconds and exits early if the client stops running; slow CI hosts may still need a rerun rather than weakening the assertion.
- `TerminalSession/StopEscalatesToKillForStubbornChild` uses `fork()` and is skipped automatically when the host blocks process creation (common in Cursor agent sandboxes). Run terminal signal tests with full permissions on a normal Linux workstation.

## Validation Loop

For meaningful work:

1. run the focused tests for the changed subsystem
2. run any broader suite needed to protect nearby ownership boundaries
3. update docs if the change altered a durable contract

If a change is hard to test, treat that as a design smell and improve the seam.

When modifying parser or persistence paths, extend at least one relevant fuzz target under
`tests/fuzz/` and add or refresh seed corpus entries under `tests/fuzz/corpora/<target>/`.
