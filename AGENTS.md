# AGENTS

## Mission

Build `microide` as a native, low-footprint, single-window C++/SDL3 desktop IDE. The target shape
is: no GPU acceleration requirement, keyboard-first workflows, and a well-validated diff / merge /
git workstation around a competent editor, search, and terminal core.

Avoid marketing claims in code, commits, or docs. The project has internal regression baselines
but no comparative benchmarks against other editors; do not write "fastest" / "lower CPU than X"
phrasing — say "native, low-footprint, responsive" and let measurements speak. See the
"Performance & Benchmark Methodology" section of `README.md` for what is actually measured.

Do not optimize for keeping old boundaries alive. If the correct fix breaks compatibility, touches
many files, or requires a broader refactor, prefer the better design.

## Product Pillars

MicroIDE's durable built-in capabilities are:

- **Editor** — text editing, UTF-8, IME, undo/redo, syntax highlighting, blame, splits
- **Diff & Merge** — compare tabs (HEAD, commits, outgoing), three-way merge tabs, shared
  row-decoration pipeline; see `openspec/specs/diff-merge-editor/spec.md` for the contract
- **Search** — async project search (literal + regex), replace-in-project, file finder
- **Git** — working-tree changes, staging, conflicts, outgoing files, blame
- **Terminal** — PTY-backed tabs with scrollback, alternate screen, full ANSI support
- **Plugins** — Lua 5.4 runtime with narrow host-owned registries for commands, sidebars,
  settings, keybindings, status items, diagnostics, hover, formatters, tasks, tools, tests,
  SCM, and auth

The authoritative product thesis, priority order, and non-goals live in
`openspec/specs/product-vision/spec.md`.

## Priority Order

When tradeoffs conflict, use this order:

1. speed
2. correctness
3. low CPU usage
4. low memory usage
5. maintainability and simplicity
6. compatibility only when it is explicitly required

Speed leads because latency is the product. That is not a licence to ship wrong behavior:
correctness sits above CPU, memory, and clarity so it is never traded for those, and a fast
path that is wrong on an input users routinely supply is not a valid solution. What the
ordering does buy is the right to bound a pathological input — a cap, a truncation, a coarser
fallback — in exchange for a materially faster common path, provided the limit is surfaced or
documented rather than silent.

Compatibility is not a default constraint. Internal APIs, temporary abstractions, and stale call
patterns can be broken or removed if that is the cleanest way to improve the system.

## Default Engineering Stance

- Prefer correct behavior over minimal diffs.
- Prefer cohesive refactors over local patches that preserve bad architecture.
- It is fine to touch many files when the change crosses ownership boundaries in reality.
- Delete dead code, stale docs, and temporary compatibility shims as soon as the new path is established.
- Keep the host small and explicit; registries and services are better extension points than giant mutable objects.

## Plugin Rules

- Do not expose `WorkspaceShell` wholesale to plugins.
- Prefer host-owned registries, commands, sidebars, and services over ad hoc plugin hooks.
- Keep rendering host-owned; plugin contributions should provide data, commands, or structured requests.
- Add async plugin execution only when real plugin workloads justify it.
- If plugin work reveals that a subsystem boundary is wrong, fix the boundary instead of layering a compatibility adapter over it.

## Performance Rules

- Speed is the primary optimization target; correctness is the floor it may not sink below.
- CPU comes before memory, especially idle CPU and redraw-path CPU.
- Measure before and after performance-sensitive changes.
- Use `dev-docs/performance/perf-harness.md` scenarios and baselines as the primary regression oracle.
- Keep `microide_perf_tests` green locally for touched scenarios and treat `perf-runner-v1` baseline checks as the authoritative gate.
- Use `dev-docs/performance/startup-tracing.md` and `dev-docs/performance/runtime-profiling.md` instead of guessing.
- Preserve typing, scrolling, resize, and startup responsiveness even when adding features.
- Prefer deleting redundant work over caching everything by default.
- The durable performance budget contract lives in `openspec/specs/performance-budgets/spec.md`.

## Architecture Rules

- Keep ownership narrow and obvious.
- Prefer small focused translation units over catch-all "shared" files.
- Push external tool and OS integration behind `src/project/*` or similarly narrow service boundaries.
- Keep UI orchestration thin; deterministic logic belongs in testable helpers.
- Avoid hidden coupling through mutable global state.
- If a coordinator grows because a subsystem lacks a real API, add the API and move logic out.

## Do-Not-Regress Patterns

The cleanups since 2026-04-29 removed patterns that had grown back more than once. The
rules that keep them out are listed once, compactly, in `CLAUDE.md` § Hard
Architectural Invariants -- each with the check in `tests/ArchitectureInvariantsTests.cpp`
/ `tests/architecture/` that hard-fails it, or marked reviewer-enforced. The full
reasoning behind each (this section's former text and CLAUDE.md's, moved verbatim on
2026-11-14) is in `dev-docs/project/architecture-invariants.md`. The durable contracts
are `openspec/specs/workspace-architecture/spec.md`,
`openspec/specs/persisted-state-format/spec.md` and
`openspec/specs/shared-edit-primitives/spec.md`; update them in the same change when an
invariant moves.

## Cleanup Passes

Duplication, smell, dead-code and god-class passes are run by many agents over months.
They only add up if they move in one direction, so a pass follows this order:

1. **Start from the record, not from taste.** Read `dev-docs/project/known-tech-debt.md`
   (open items and § Guardrails -- rejected experiments) and
   `dev-docs/project/architecture-invariants.md` before choosing targets. A target that
   contradicts either is out of scope unless the pass changes the record on purpose.
2. **Check for oscillation.** Run `python3 tools/refactor-churn.py --since=<last pass>`.
   A pair of commits that keep trading the same lines is a disagreement: settle it once,
   write the reason down, and encode it (next step) instead of picking a side again.
   Find targets with the gated scanners (`tools/clone-scan.py`,
   `tools/duplicate-constant-scan.py`) before eyeballing.
3. **Encode every settled direction in the same change.** When a pass decides "X, not Y"
   (a shape, a name, a boundary), it adds or tightens an architecture rule in
   `tests/architecture/` -- a ban, or a count that ratchets DOWN -- with fixtures both
   ways and a missing-target guard, lists it in `CLAUDE.md` § Hard Architectural
   Invariants, and puts the why in `architecture-invariants.md`. Prose alone does not
   survive the next agent; a failing check does. Lower a ratchet's cap in the change that
   earns it; never raise one to make a change fit.
4. **Leave the record current.** Update the TD entry the pass worked on (what landed, what
   was left on purpose and why) so the next pass starts where this one stopped.

## Testing Rules

- Every meaningful bug fix should add or tighten regression coverage.
- Run targeted builds and tests for the changed area before committing.
- For the inner loop, build `--target microide_tests microide_kernel_tests` and run `ctest -R _tests_shard` (the two test binaries; skips `microide` + benches). Kernel tests (util/platform/project/compare/persistence/terminal) live in the SDL-free `microide_kernel_tests`. The build auto-detects `ccache` + `ld.lld` when present; core is shared once via the `microide_core` object library. Test seams are runtime-gated (`terminal::SetUsePlaceholderTerminalsForTesting`, `platform::SetHostPlatformOverrideForTesting`), NOT compile-time `#ifdef MICROIDE_TESTING` — keep core free of `MICROIDE_TESTING` branches so it stays ABI-identical across the production and test binaries.
- Always run the suite and sanitizers through `tools/run-checks.sh {tests|asan|ubsan|tsan|all}` — never raw `ctest` or the sanitizer binary directly. The wrapper tees build+test output to `/tmp/microide-<target>.log` (read that instead of rerunning) AND sets the sanitizer runtime options, notably `TSAN_OPTIONS=...:suppressions=tests/tsan.supp`. Running `ctest` directly skips the suppressions, so environmental races (Mesa `libgallium` GPU driver via `ApplicationTests`, libdbus) surface as false failures and you waste a second run capturing output.
- Run sanitizer variants (`microide-asan`, `microide-ubsan`, `microide-tsan`) for memory/thread-sensitive changes.
- TSAN needs ASLR entropy below the kernel default or it aborts at startup with `unexpected memory mapping`. `run-checks.sh tsan` handles this itself by running ctest under `setarch -R`, which clears ASLR for that process tree via `personality(ADDR_NO_RANDOMIZE)` — no root, and nothing changed machine-wide. Driving ctest by hand needs the same prefix. `sudo sysctl vm.mmap_rnd_bits=28` remains the fallback for sandboxes that block that personality bit; the wrapper prints which path it took.
- If a genuinely environmental (non-microide) race or leak appears, add a documented suppression to `tests/tsan.supp` (mirroring the existing `deadlock:libdbus-1` / `race:libgallium` entries) rather than leaving a flake in the matrix.
- Extend and run relevant fuzz targets in `tests/fuzz/` when changing persistence, parser, regex, or blame decode paths.
- A test that dispatches a SYNTHETIC file-watcher batch (`DispatchFileIndexWatcherBatchForTesting`) and asserts on the result must first call `WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting`, which is `FileIndexWatcher::Unwatch()` — it stops the native/poll threads while keeping the watcher and its callback, and joins them so nothing is in flight during the assertions. Otherwise the live inotify watcher reacting to the fixture's own writes is a second, uncontrolled source of batches, arriving before, during, after, or not at all inside the test's window (TD-2026-09-29-311). Tests that deliberately wait on a REAL watcher event keep the watcher alive and must not call it.
- Redraw comparison tests under SDL dummy video should run serially.
- Use focused fixtures for git, search, compare, merge, and plugin-adjacent workflows.
- If a change is hard to test, treat that as a design smell and improve the seam.
- CI (`.github/workflows/checks.yml`) runs the same `tools/run-checks.sh` targets on every push and pull request: `tests`, `perf-tests`, `perf-canary`, `clang-build`, `asan`, `ubsan`, `tsan`, and a build-and-smoke `fuzz` pass. Because every job shells out to the wrapper, a red CI run reproduces locally with the exact command in its uploaded log. `run-checks.sh all` covers `tests`, `clang-build`, `perf-canary` and the three sanitizers — **not** `perf-tests`, `fuzz` or `coverage`, so a green `all` is a strong signal but not a proof of a green CI. Keep new validation behind `run-checks.sh` rather than adding a bespoke CI step, or the two paths drift and the local run stops meaning anything.
- The `fuzz-smoke` job exists specifically because fuzz targets list explicit sources instead of linking `microide_core`: adding a dependency to a shared `.cpp` breaks their link, and no default build flow compiles them, so that break is otherwise silent.
- The `clang-build` job is the second compiler AND the only lane that treats warnings as errors. Every other lane is GCC (the sanitizer presets set no compiler, so all three use `/usr/bin/c++`), `coverage` builds only `microide_tests`, and `fuzz-smoke`'s clang builds twelve explicit-source targets — so `src/app/main.cpp`, `microide_perf` and the benches had one compiler between them, and a whole-tree clang break has reached `main` before. `MICROIDE_WARNINGS_AS_ERRORS` defaults OFF and every preset pins it OFF, which left the extra-warning ratchet in `CMakeLists.txt` documented but unenforced; this job is its pawl.
- The `perf-canary` step is a vacuity probe for the perf gate itself, in the spirit of the architecture lint's positive-control fixtures. It runs `perf_gate_canary` clean (must pass) and inflated 4x (must **fail**); an inflated run that passes means every baseline in `tests/perf/baselines/` is silently unenforced. This matters because the suite's wall numbers have already been wrong twice for environmental reasons (the xvfb video lane, hybrid-CPU placement) and both times looked exactly like code regressions. `PerfBaselineTests` proves the comparison arithmetic; only this proves the pipeline feeding it. The canary's allocation count is fixed by its own source, so the assertion holds on any machine — unlike the real baselines, it is portable to a hosted runner.
- Perf baselines are absolute timings from the pinned `perf-runner-v1` machine, so CI deliberately does not re-measure them (a hosted runner would fail all 92). What CI does enforce is `tools/check_perf_baseline_tag.sh`: a PR that edits a baseline JSON must carry a `perf-baseline:` line saying why. Rebaselining stays a local, deliberate act.

## Documentation Rules

- Keep `dev-docs/project/active-work.md` current when priorities or shipped status change.
- Keep `dev-docs/project/implementation-guide.md` aligned with durable product direction.
- Update subsystem design docs when a change materially alters the intended architecture.
- Remove stale or split-brain docs rather than leaving contradictory guidance around.
- When a durable policy changes, update the relevant `openspec/specs/` file in the same commit.

## Iteration Loop

The default loop is:

1. implementation
2. tests
3. docs
4. commit

Repeat in coherent slices. Do not leave tests, docs, or commits as an afterthought.

## Commit Discipline

- Prefer coherent commits over giant mixed snapshots.
- Each commit should represent a defensible step forward.
- If the right fix is broad, make it broad but still keep the commit logically unified.
- Mention major refactors plainly in the commit message instead of hiding them behind vague wording.
