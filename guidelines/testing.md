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
