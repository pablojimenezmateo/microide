## Context

A survey of `/home/pablo/Downloads/tmp/godot-master/` against microide's stated priorities and architecture surfaced six concretely applicable patterns. The remaining Godot patterns either solve game-engine-scale problems microide does not have, or directly conflict with microide's "narrow services, no god-objects, host-owned rendering" stance.

Current state in microide:
- Editor gutters are wired per feature (blame, diagnostics, breakpoints, fold marks). Adding a new gutter today requires touching the editor render path directly.
- Text measurement is performed in places along the render path. There is no formal "measurement is forbidden during paint" invariant in code, only in convention.
- Preconditions are checked ad hoc: some sites use plain `if (...) return ...;`, some use `assert`, some throw. The repo already bans throwing parse paths via `util/Parse.h`.
- Tests assert on final state but not on the sequence of intermediate notifications/events. Negative-path tests that intentionally trigger logs print noisily into the test summary.
- CI runs full-tree static checks on every PR. The architectural-lint test enforces the load-bearing invariants but pure-style regressions (header guard form, file format, include path style) are not caught.
- A `.clang-tidy` is not present.

The lessons being adopted come from these specific Godot locations:
- `scene/gui/text_edit.h` — `add_gutter` / `set_gutter_type` / `set_gutter_custom_draw` API and the per-line `Ref<TextParagraph>` with `lines_dirty`.
- `core/error/error_macros.h` — non-throwing `ERR_FAIL_*` macros + runtime-toggleable error printing.
- `tests/test_macros.h` — `ERR_PRINT_OFF/ON` scope helpers; `tests/signal_watcher.h` — recorder for emitted signals between checkpoints.
- `misc/scripts/header_guards.py`, `misc/scripts/file_format.py`, `misc/scripts/validate_includes.py` — Python lint scripts run from CI.
- `.clang-tidy` — small curated list (~8 checks) plus a `HeaderFilterRegex` to scope the run.
- `.github/workflows/static_checks.yml` — `tj-actions/changed-files` to scope checks to PR diff.

## Goals / Non-Goals

**Goals:**

- Editor: a single `GutterRegistry` is the only seam for adding a column. The render path becomes a consumer of cached, per-line measurement data — never a producer of it.
- Tooling: a small `MICROIDE_FAIL_*` macro family is the standard precondition tool, with debug-build asserts and release-build log+return; runtime-toggleable so test failure-path coverage is quiet.
- Tests: a scoped `LogSilencer` for negative-path tests; an `EventRecorder` that captures host-service notifications between checkpoints so behavior tests can assert on sequences.
- CI/style: three Python lint guards + a tiny `.clang-tidy` profile, both wired so regressions are caught early; static-checks workflow scoped to changed files for fast PR feedback.
- Architecture invariants: the existing `tests/ArchitectureInvariantsTests.cpp` is extended so that the "no measurement during paint" rule and "no direct gutter draw outside `GutterRegistry`" rule are hard-fail-on-regression.

**Non-Goals:**

- Replacing `std::string`/`std::string_view`/`std::vector` with custom Godot-style containers (`String`, `StringName`, `CowData`, `LocalVector`).
- Adding any reflection / dynamic-dispatch layer (`Object`/`Variant`/`Callable`/signals).
- Switching the build to a single-compilation-unit (SCU) builder.
- Replacing the existing `ProjectBackgroundExecutor` with a Godot-style `WorkerThreadPool`.
- Introducing a translation server / i18n framework.
- Changing the Lua plugin ABI shape — only adding a typed `gutter.add(...)` style entry that routes through the new registry.
- New runtime third-party dependencies.
- Performance work that requires profiler-grounded justification (e.g., paged allocators, `resize_uninitialized` helpers): explicitly deferred to watch-list, not part of this change.

## Decisions

### Decision 1: One `GutterRegistry` host service, indexed by stable id

`GutterRegistry` lives alongside the other workspace services (peer of `EditorTabService` and friends, see CLAUDE.md "Hard Architectural Invariants"). It is constructed with the service interfaces it needs and never references `WorkspaceShell&`. Columns are added by id and described by a small typed enum (TEXT, ICON, CUSTOM_DRAW). For CUSTOM_DRAW, the contributor passes a `std::function<void(GutterDrawContext&, LineIndex)>` that receives a clipped, line-local draw context.

Alternatives considered:
- *Keep per-feature wiring, just document the convention.* Rejected — that is the current state, and it has resulted in render-path coupling for every new gutter contributor.
- *Make gutters a plugin-only concept.* Rejected — host features (diagnostics, blame) need the same column model and would otherwise reimplement layout.
- *Use an inheritance-based `IGutterContributor` with a vtable.* Rejected per `guidelines/cpp.md` — composition + typed enum + closure suffices and avoids a polymorphic boundary that would not be reused elsewhere.

### Decision 2: Per-line measurement cache lives on the existing line type, with explicit `measure_dirty`

Each line in the editor text buffer gains a small POD measurement cache (pixel width, glyph runs / wrap break offsets as already produced by `TextRenderer`). Mutators flip `measure_dirty = true` for affected lines only. A `EnsureMeasured(LineIndex)` helper performs the SDL3_ttf measurement once and clears the flag. The render path reads only from the cache and may not call measurement APIs.

Alternatives considered:
- *Separate side-table keyed by line id.* Rejected — adds indirection and a second invalidation path; each line already owns its content, so co-locating the cache is simpler.
- *Eager re-measure on every edit.* Rejected — wasteful for offscreen lines and makes large-file edits worse.
- *Lazy re-measure inside the render path itself.* Rejected — that is exactly the current implicit behavior we want to forbid; allowing it makes redraw timing non-deterministic and complicates perf-harness measurements.

The "no measurement during paint" invariant is enforced by extending the architectural-lint test to grep render TUs for the measurement entry-point names (the same pattern already used to forbid `context_.current_project_state` in render TUs).

### Decision 3: `MICROIDE_FAIL_*` macros assert in debug, log+return in release, runtime-toggleable

Header `src/util/Fail.h` provides a tiny family:

- `MICROIDE_FAIL_INDEX_V(index, count, retval)` — returns `retval` (and asserts in debug) if `index >= count`.
- `MICROIDE_FAIL_COND_V(cond, retval)` — returns `retval` if `cond` is true.
- `MICROIDE_FAIL_NULL_V(ptr, retval)` — returns `retval` if `ptr == nullptr`.
- Void variants (`MICROIDE_FAIL_INDEX`, `MICROIDE_FAIL_COND`, `MICROIDE_FAIL_NULL`) for `void`-returning functions.

Each macro logs through the existing `Log` infrastructure with file:line, behind a thread-local `g_log_failures` flag toggleable via the new `LogSilencer` RAII guard.

Alternatives considered:
- *Throw exceptions.* Rejected — repo policy already bans throwing in these paths (see `util/Parse.h`).
- *Use `assert` only.* Rejected — assert is compiled out in release; release behavior would silently dereference / out-of-bound. This family makes release behavior defined and observable.
- *Use `std::expected`.* Rejected for now — would require refactoring every caller signature; the macro family is a drop-in for the existing `if (...) return ...;` pattern. `std::expected` remains an option for return-value-rich APIs and is not blocked by this change.

### Decision 4: `LogSilencer` RAII guard + `EventRecorder` test helper

`tests/util/LogSilencer.h`: scoped guard that flips `g_log_failures` to false on construction and restores on destruction.

`tests/util/EventRecorder.h`: a recorder that subscribes to a chosen host service's notification surface (e.g., `EditorTabService::OnNotification(...)`) and records every event into a vector with a stable to-string serialization, so tests can assert with `EXPECT_EQ(recorder.Events(), {...})` rather than re-querying state. The recorder is a host-side test utility — no production code changes; services are already designed to emit notifications because microide tests already observe them via final state.

Alternatives considered:
- *Pipe logs through a `std::ostringstream` per test.* Rejected — verbose, easy to forget reset, no help for sequence-of-events checks.
- *Add a global logger fixture.* Rejected — global mutable test state has caused the existing redraw tests to require serial execution; a stack-scoped guard avoids the same trap.

### Decision 5: Three small Python lint guards + a tiny `.clang-tidy`, scoped via `tj-actions/changed-files`

Add `scripts/lint/check_header_pragma.py`, `scripts/lint/check_file_format.py`, `scripts/lint/check_include_form.py`. Each takes a list of paths on argv (or finds all matching files when invoked with no args) and exits non-zero on violation. CTest gains targets `lint-header-pragma`, `lint-file-format`, `lint-include-form` so they are also locally runnable through the existing build/test workflow.

`.clang-tidy` enables an explicit short list (no globs):

- `bugprone-use-after-move`
- `bugprone-unchecked-optional-access`
- `performance-move-const-arg`
- `performance-unnecessary-value-param`
- `readability-braces-around-statements`
- `readability-redundant-member-init`
- `modernize-use-nullptr`
- `modernize-use-override`

`HeaderFilterRegex` scopes the run to `^(src|tests)/`. The clang-tidy CI job is **non-blocking** initially (warn-only) and is promoted to blocking in a follow-up change once the codebase is clean.

A new GitHub Actions job (or a new step in the existing static-checks workflow) uses `tj-actions/changed-files@v45` (or local equivalent) to compute the changed file list and passes it to the lint scripts and clang-format. The architectural-lint test continues to run unscoped because it inspects multi-file invariants.

Alternatives considered:
- *One mega-lint script.* Rejected — three small scripts are easier to test, easier to disable individually, and match Godot's split that has held up over years.
- *Adopt the full clang-tidy default check set.* Rejected — Godot deliberately runs a tiny curated set; the full set produces noise that hides high-signal warnings.
- *Use `pre-commit` framework upstream.* Deferred — microide does not currently use pre-commit; introducing it is a larger change. The Python scripts run fine from CTest and CI without it. Adopting `prek`/pre-commit is on the watch-list.

### Decision 6: Migration is incremental, in two phases

Phase A (this change):
- Land `GutterRegistry`, `MICROIDE_FAIL_*`, `LogSilencer`, `EventRecorder`, the three Python lints, the `.clang-tidy`, and the changed-files CI scoping.
- Migrate **one** existing gutter contributor (blame is the simplest) onto `GutterRegistry` to validate the API.
- Add the per-line measurement cache and migrate one render TU to consume it; the architectural-lint extension lands in warn-only mode for this phase.

Phase B (follow-up change, **out of scope here** but described so reviewers see the arc):
- Migrate the remaining gutter contributors (diagnostics, breakpoints, fold marks).
- Migrate all render TUs to consume the measurement cache; the architectural-lint extension flips to hard-fail.
- Promote clang-tidy CI from warn-only to blocking.
- Open a watch-list issue for the deferred picks (allocator helper, paged allocator, doctest force-link, etc.) gated on profiler evidence.

## Risks / Trade-offs

- [The gutter-registry abstraction introduces an API surface that calcifies before all real contributors are migrated.] → Mitigation: Phase A migrates exactly one contributor (blame) end-to-end before the registry is considered stable. The delta spec only requires Phase A; Phase B is a separate change.
- [The per-line measurement cache invariant is a new "no measurement during paint" rule that could block legitimate work.] → Mitigation: the lint test runs in warn-only mode for Phase A. Render TUs that today need to measure are migrated before the lint flips to fail.
- [`MICROIDE_FAIL_*` macros are a subtle change to release-build behavior at every adoption site (silent UB → defined log+return).] → Mitigation: adoption is opt-in per call site; the existing ad-hoc `if (...) return ...;` pattern keeps working unchanged. We do not mass-rewrite call sites in this change.
- [`LogSilencer` is a global thread-local flag; misuse (forgetting it crosses a test boundary) could mask real failures.] → Mitigation: RAII scope, restored in destructor; unit-tested with a dedicated test that asserts the flag is restored on scope exit.
- [The clang-tidy CI job, if mis-configured, can spam PRs with warnings.] → Mitigation: starts non-blocking and explicitly warn-only, with the curated short check list. Promotion to blocking is a separate change after a clean run is observed.
- [`tj-actions/changed-files` was the subject of a 2025 supply-chain incident.] → Mitigation: pin to a commit SHA, not a tag; review the action source; or substitute with `git diff --name-only` against the merge base inline (a 5-line shell snippet) and avoid the action entirely. The implementation tasks call this out.
- [Plugin breakage from the gutter API change.] → Mitigation: there are no known third-party Lua plugins drawing custom gutters today; the **BREAKING** flag is precautionary. The Lua-side gutter helper is added in the same change so any in-tree plugin migrates atomically.
- [The architectural-lint extension uses regex over render TUs and could miss measurement calls hidden behind a typedef or local alias.] → Mitigation: keep measurement APIs uniformly named (already true) and add a unit test for the lint itself that feeds it a positive and negative fixture.

## Migration Plan

1. Land all six items behind their respective code paths; the registry has no contributors yet, the cache has no readers yet, the macros have no adopters yet.
2. Migrate the blame gutter to `GutterRegistry`. This is the canary; no other migration begins until blame is green.
3. Migrate one render TU to read from the per-line measurement cache. Verify the perf harness shows no regression and ideally a small scroll-CPU win.
4. Enable the three Python lints in CTest (locally enforced), then enable in CI in warn-only mode for one PR cycle, then flip to blocking.
5. Land `.clang-tidy` and the CI job in warn-only mode.
6. Land the changed-files CI scoping. Verify a PR sees only its own files in the lint output.
7. Open the Phase B follow-up change ticket capturing the remaining migrations and the lint-test promotion.

Rollback: each item is independently revertible. If `GutterRegistry` proves wrong, blame can be reverted to its current wiring without affecting the other five items. The Python lints can be disabled in CI by removing the workflow step. The `.clang-tidy` can be deleted with no code impact.

## Open Questions

- Does `GutterRegistry` belong in `src/editor/`, in `src/workspace/` alongside the other services, or in a new `src/editor/services/` directory? Tasks pick `src/editor/` for now; the workspace-architecture spec already requires services to live alongside the shell, so the final placement may want a short delta there if the registry truly is workspace-level. Resolve during Phase A implementation.
- Should the `EventRecorder` be one helper that adapts to any service, or one helper per service? Lean toward one templated helper plus per-service `Trait` specializations so to-string serialization is centralized. Defer to implementation.
- The `tj-actions/changed-files` vs. inline `git diff` choice: tasks default to the inline `git diff` to avoid the supply-chain footgun. Confirm during PR review.
- Do we want the `MICROIDE_FAIL_*` macros to support a printf-style message, or string-only, or no message? Lean toward a single `const char* msg` parameter (no formatting) to keep the macros tiny and the log call boring.
