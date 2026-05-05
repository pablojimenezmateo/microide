## 1. Foundations: precondition macros + log silencer

- [ ] 1.1 Add `src/util/Fail.h` defining `MICROIDE_FAIL_INDEX_V`, `MICROIDE_FAIL_COND_V`, `MICROIDE_FAIL_NULL_V`, plus the `void`-returning variants; debug build asserts, release build logs through existing `Log` infrastructure and returns
- [ ] 1.2 Add a thread-local `g_log_failures` flag (default `true`) consulted by every macro before emitting a log entry
- [ ] 1.3 Add `tests/util/LogSilencer.h` RAII guard that flips and restores `g_log_failures`; ensure nesting is safe (capture prior value, restore in destructor)
- [ ] 1.4 Add `tests/util/LogSilencerTests.cpp` covering: single-scope suppression, scope-exit restoration, nested guards, no impact when no failure macro fires
- [ ] 1.5 Adopt `MICROIDE_FAIL_*` at three pilot sites in code already using `if (...) return ...;` precondition checks (pick sites in `src/util/` and `src/editor/` for low blast radius)
- [ ] 1.6 Update `guidelines/cpp.md` to document `MICROIDE_FAIL_*` as the standard precondition tool, when to prefer it over `assert` / `std::optional` / `std::expected`

## 2. EventRecorder test helper

- [ ] 2.1 Add `tests/util/EventRecorder.h` as a templated helper parameterised on a service notification trait that exposes a `Subscribe(callback)` / `Unsubscribe(token)` pair
- [ ] 2.2 Add a `Trait` specialisation for `EditorTabService` notifications with a stable to-string serialisation
- [ ] 2.3 Add `tests/util/EventRecorderTests.cpp` covering: capture order, RAII unsubscribe, no recording after destruction, deterministic serialisation
- [ ] 2.4 Convert one existing `EditorTabService` test from final-state assertion to event-sequence assertion as the canary
- [ ] 2.5 Update `guidelines/testing.md` with a section "Asserting on dispatched event sequences" referencing the recorder

## 3. Per-line measurement cache + render-path invariant

- [ ] 3.1 Identify the editor line type (current home of per-line storage) and add a `MeasureCache` POD field plus a `bool measure_dirty` flag, both default-initialized to "dirty / empty"
- [ ] 3.2 Audit every mutator that changes line content; on every such mutator, set `measure_dirty = true` for the affected lines only (do not mass-invalidate)
- [ ] 3.3 Add `EnsureMeasured(LineIndex)` helper on the line cache owner that runs SDL3_ttf measurement once when `measure_dirty` is true and clears the flag
- [ ] 3.4 Migrate exactly one render TU to read pixel width / glyph runs from `MeasureCache` instead of measuring during paint; verify identical output via existing redraw comparison tests
- [ ] 3.5 Add an architectural-lint case (warn-only in this change) in `tests/ArchitectureInvariantsTests.cpp` that flags any render TU referencing the forbidden measurement entry-point names; include a positive and a negative fixture
- [ ] 3.6 Run `docs/perf-harness.md` scroll scenario before and after; record results in the change journal (no regression required; ideally a small win)

## 4. GutterRegistry — host service

- [ ] 4.1 Decide final placement (`src/editor/GutterRegistry.{h,cpp}` versus `src/workspace/`); resolve the open question in `design.md` § Open Questions before writing code
- [ ] 4.2 Implement `GutterRegistry` constructor taking only narrow service-interface refs (measurement-cache reader, notification emitter); explicitly NOT `WorkspaceShell&`/`WorkspaceShell*`
- [ ] 4.3 Implement `AddColumn(GutterDescriptor)` / `RemoveColumn(id)` returning a stable handle; `RemoveColumn` of unknown id is no-op returning `false`
- [ ] 4.4 Implement the typed `kind` enum (`TEXT`, `ICON`, `CUSTOM_DRAW`) with the per-kind provider signatures from the spec
- [ ] 4.5 Implement layout: registry maintains an ordered list of columns; ordering is registration order unless an explicit `position` is passed (deferred — registration order is enough for Phase A)
- [ ] 4.6 Implement `GutterDrawContext` that exposes only the clipped per-line draw surface for `CUSTOM_DRAW` columns; SHALL NOT expose the underlying editor surface or shell

## 5. GutterRegistry — render-path integration

- [ ] 5.1 Migrate the editor render path so that the gutter strip is iterated through `GutterRegistry` rather than by direct per-feature wiring
- [ ] 5.2 Confirm hit testing for click-on-gutter routes through the registry (necessary for breakpoint toggles in Phase B; surface the hook now even if no contributor uses it yet)
- [ ] 5.3 Migrate the **blame** gutter onto `GutterRegistry` end-to-end; remove the previous direct draw code; update or add fixtures so blame still appears in the existing tests
- [ ] 5.4 Verify diagnostics, breakpoints, and fold marks (still on legacy wiring per Phase A) continue to display unchanged
- [ ] 5.5 Add `GutterRegistryTests.cpp` covering: add/remove, ordered iteration, unknown-id remove returns `false`, `CUSTOM_DRAW` callback receives a clipped context

## 6. GutterRegistry — Lua plugin API

- [ ] 6.1 Add `microide.gutter.add{ id, kind, width, provider }` to the Lua plugin host, routing to `GutterRegistry::AddColumn`
- [ ] 6.2 Track per-plugin column ids; on plugin unload, remove every column registered by that plugin (no leaks across reloads)
- [ ] 6.3 Add Lua test plugin fixture exercising a `TEXT` column add/remove and verify it appears/disappears
- [ ] 6.4 Update `guidelines/plugins.md` documenting the gutter API as the only seam for adding columns

## 7. Python lint scripts

- [ ] 7.1 Add `scripts/lint/check_header_pragma.py` — accepts a list of paths on argv, walks repo headers if no args, exits non-zero on missing `#pragma once` or on classic `#ifndef`/`#define`/`#endif` guards
- [ ] 7.2 Add `scripts/lint/check_file_format.py` — verifies no UTF-8 BOM, LF-only line endings, no trailing whitespace, exactly one trailing newline
- [ ] 7.3 Add `scripts/lint/check_include_form.py` — forbids `\` in include paths, forbids `..` segments, requires double quotes for repo-internal headers, allows quotes for vendored headers
- [ ] 7.4 Add a small `scripts/lint/_common.py` for shared helpers (tracked-file enumeration via `git ls-files`, exit-code printing) and unit-test it
- [ ] 7.5 Add fixture-based tests for each lint script under `scripts/lint/tests/` (positive + negative inputs)
- [ ] 7.6 Wire each lint as a CTest case (`lint-header-pragma`, `lint-file-format`, `lint-include-form`) so `ctest` runs them locally

## 8. clang-tidy profile

- [ ] 8.1 Add `.clang-tidy` at repo root with the curated short list: `bugprone-use-after-move`, `bugprone-unchecked-optional-access`, `performance-move-const-arg`, `performance-unnecessary-value-param`, `readability-braces-around-statements`, `readability-redundant-member-init`, `modernize-use-nullptr`, `modernize-use-override`
- [ ] 8.2 Set `HeaderFilterRegex` to scope to `^(src|tests)/`
- [ ] 8.3 Verify CMake produces `compile_commands.json` (already true under default config) and document the local invocation `clang-tidy -p build src/...` in `guidelines/testing.md` or a new `docs/static-analysis.md`
- [ ] 8.4 Run clang-tidy locally over the existing tree and capture the warning baseline; record it in the change journal so PRs do not regress relative to baseline
- [ ] 8.5 Fix any warnings deemed trivial within scope of this change (do not chase a clean tree — that is Phase B)

## 9. CI — static-checks workflow

- [ ] 9.1 Add or extend `.github/workflows/static_checks.yml` with a new job that computes the changed file list using inline `git diff --name-only origin/${{ github.base_ref }}...HEAD` (avoids the third-party action / supply-chain risk per design.md § Decision 5)
- [ ] 9.2 Run `clang-format --dry-run --Werror` against the changed C/C++ files only
- [ ] 9.3 Run the three Python lints (`check_header_pragma`, `check_file_format`, `check_include_form`) against the changed files only
- [ ] 9.4 Run clang-tidy against the changed files using `compile_commands.json`; mark the step `continue-on-error: true` for this change (warn-only) and document the promotion plan in the change journal
- [ ] 9.5 Confirm the architectural-invariant test continues to run unscoped (full repository) in its existing CI job
- [ ] 9.6 Open a follow-up issue/change for "Promote clang-tidy to blocking" and "Phase B: migrate remaining gutters + flip render-path lint to fail" so the deferred work has a tracking home

## 10. Validation

- [ ] 10.1 Run `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- [ ] 10.2 Run sanitizer presets (`microide-asan`, `microide-ubsan`, `microide-tsan`) and confirm clean
- [ ] 10.3 Run the relevant `docs/perf-harness.md` scenarios (scroll, large-file open) — capture before/after numbers in the change journal
- [ ] 10.4 Run `openspec validate godot-lessons-adoption --strict` and resolve any reported issues
- [ ] 10.5 Update `docs/active-work.md` to note the adoption phase is in progress; link to this change directory
- [ ] 10.6 Smoke-test the binary: open a project, exercise blame gutter, exercise diagnostics gutter, scroll a large file, load a Lua plugin that adds a gutter column, verify no log noise and no measurable scroll regression
