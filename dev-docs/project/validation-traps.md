# Validation Traps

How to avoid concluding the tree is healthy when it is not. Every item here cost
real time at least once; several were green checkmarks over live defects.

The theme: **a passing check is only evidence if the check could have failed.**
Most entries below are ways that stopped being true without anyone noticing.

## Build And Test Result Traps

### A failed build leaves ctest reporting 100%

`cmake --build build` failing does **not** stop `ctest --test-dir build` from
running. The previous `microide_tests` binary is still on disk, so ctest reports
`100% tests passed, 0 tests failed out of 24` against code that no longer
compiles. Hit on 2026-07-27 after a symbol deletion: four compile errors, then a
clean green ctest run.

It is easy to walk into because the usual habit is piping the build through
`grep -E "error:"` to keep output short — which discards the exit status, and the
next command then reads as confirmation. Sharded ctest (24 processes) makes the
green look especially authoritative.

- Never conclude from a ctest result alone. Check the build's real status
  (`cmake --build … ; echo rc=$?` — not `${PIPESTATUS}` after a grep that already
  swallowed it), or confirm it printed `Built target microide_tests` with no
  error lines.
- `tools/run-checks.sh` already does this correctly: it `set -e`s build and test
  into one `bash -c`, so a build failure fails the run. Prefer it for anything
  conclusive.

### Editing source while a build or sanitizer run is in flight

This produces an object set where some TUs saw the old class layout and some the
new. `microide_core` is a shared object library across
`microide`/`microide_tests`/`microide_perf`, so one changed header contaminates
everything linked from it.

The symptom is not a link error. It is a plausible-looking sanitizer report:

```
/usr/include/c++/13/optional:473:58: runtime error: load of value 80,
    which is not a valid value for type 'bool'
/usr/include/c++/13/bits/stl_construct.h:163:19: runtime error:
    reference binding to null pointer of type 'struct basic_string'
LspService.cpp:320:20: runtime error: member access within null pointer
    of type 'struct WorkspaceContext'
0% tests passed, 24 tests failed out of 24   (one SEGFAULT)
```

Every one of those was an artifact (2026-08-03: two members removed from a cache
struct while the UBSAN shards were running).

- Treat a sanitizer or full-build run as a barrier. Finish every edit, confirm
  `git status` is clean, then start the run.
- The tell that a report is an artifact is **breadth**: many shards, unrelated
  files, garbage values inside `std::optional`/`std::string` internals. A real
  finding is usually one site.
- On suspicion, `rm -rf build/microide-{asan,ubsan,tsan}` and re-run rather than
  rebuilding incrementally. ccache makes the from-scratch rebuild cheap.

### `git checkout <file>` on a `git mv`'d file restores pre-rewrite content

During a large move, `git checkout` on a moved file restores the version from the
index — which is the pre-rewrite content, silently undoing include fixups. Hit
during the 312-file `src/workspace` split.

## Architecture-Lint Vacuity

A green `ArchitectureInvariants` run is **not** evidence the tree is clean. On
2026-07-26 several hard rules were passing because their pattern could never
match: three were structurally dead and two were hiding real defects (an
unflagged `open()` without `O_CLOEXEC`, and a synchronous `git rev-parse` on the
shell thread).

Causes seen so far:

- `CheckDescriptorCreationIsCloseOnExec` spelled its pattern `openat?` — the `?`
  binds to the preceding `t`, so it matched `opena`/`openat` and never plain
  `open(`.
- `CheckTerminalSessionNoExtractedImpl` used `^` without `std::regex::multiline`,
  which in `std::regex` means offset 0 of the whole file.
- `CheckNoDirectGitRepositoryInWorkspace` matched only `GitRepository(` — the
  temporary form nobody writes — while every real site is `GitRepository repo(…)`.

### Three vacuity vectors

1. **Stale fixtures.** A rule's meta-fixture writes files into a temp root. When
   the rule's target path moves, `ReadText` returns `""`, the rule emits a "could
   not locate body" violation, and a negative-only `Expect(!violations.empty())`
   passes without ever exercising the pattern scan.
2. **Stale explicit paths in the rule itself.** A stale-path audit on 2026-07-24
   found two silently vacuous rules: `CheckPersistenceFileIoBoundary` (all four
   exemption paths named deleted files) and `CheckStatusBarRefreshIsAsyncOnly`
   (scanned a retired TU; `if (!exists) return result;` = silent pass).
   Now mitigated: `RequireRuleTarget` / `ReadRuleTarget` record `missing_targets`
   and the real-repo run fails on those as well as on violations.
3. **Non-recursive `directory_iterator` — the only silent one left.** Twelve
   rules enumerated `directory_iterator(repo_root / "src/workspace")` and
   selected by filename. An enumerating rule that simply *sees fewer files* has
   nothing to report: it stays green while covering less. They were already blind
   to `src/workspace/testaccess/` before anything moved. Converting all twelve to
   `recursive_directory_iterator` is what let the 312-file subsystem split land
   safely (`bc30036e`) — do that **first**, before moving anything.

### Probing a rule

No rebuild needed; the test binary reads the live tree.

```bash
cp "$file" /tmp/probe.bak
printf '%s\n' "<synthetic violation>" >> "$file"
./build/microide/microide_tests "ArchitectureInvariants/Workspace/$rule"
cp /tmp/probe.bak "$file"
```

- Craft the probe from the rule's **actual regex**, not its comment — several
  rules are narrowly scoped and a plausible-looking probe just misses. The
  coordinator rule builds its regex from the file *stem*, so a probe naming a
  different type proves nothing.
- Detected-but-green means `result.hard_fail` was never set (warn-only).
- For required-presence rules, delete the anchor token instead of appending.
- After moving files, probe at the **new** path, one probe per rule family.
- Stale-path audit one-liner, worth re-running after any lint edit or `src/`
  rename: extract `repo_root / "…"` and quoted `"src/…"` literals from
  `tests/architecture/*.cpp` and check each for existence.

### Writing a rule that stays honest

- Fixtures go in `tests/architecture/ArchitectureRuleFixtures.cpp`, not
  `ArchitectureInvariantsTests.cpp` (capped as a dispatcher at 320 code lines).
- Always add a **positive control**: a clean fixture asserted to produce zero
  violations. A negative-only check cannot distinguish "rule caught the bad
  pattern" from "rule could not find the file at all".
- Gate every named target on `RequireRuleTarget`/`ReadRuleTarget`, and make a
  rule that finds *no* call sites at all report that as a violation rather than
  passing vacuously.
- When a lint enforces A-implies-B over two lists, ask whether B-implies-A is
  also a bug. For settings it was the worse one — see below.

## Perf-Gate Vacuity

The lint is not the only instrument that can go quietly blind. The perf gate has
the same shape of problem and, unlike the lint, a track record: this suite's wall
numbers have been silently wrong twice for purely environmental reasons — an
xvfb-wrapped video lane inflating frame-pumping scenarios 2-12x, and hybrid-CPU
placement adding another 2.4x. Both looked exactly like code regressions. Both
cost real sessions before the lane was identified.

`PerfBaselineTests` covers `CompareToBaseline` with hand-written numbers. That
proves the comparison arithmetic and nothing about the pipeline feeding it: if
measurement, aggregation, baseline loading or the process exit code breaks, every
scenario reports a clean run, and a clean run is indistinguishable from a real
pass.

### The probe

`tools/run-checks.sh perf-canary` runs `perf_gate_canary`
(`tests/perf/PerfGateCanaryScenario.cpp`) twice and asserts **both** directions:

| run | expected | what a wrong result means |
| --- | --- | --- |
| clean | PASS | a failure means the committed baseline does not describe this machine, so the probe concludes nothing |
| inflated 4x | **FAIL** | a pass means the gate has stopped gating and every baseline in `tests/perf/baselines/` is unenforced |

The second direction is the entire point, and it is the direction ordinary checks
never test. Verify the probe itself the same way you verify a lint rule — run it
with `MICROIDE_PERF_CANARY_FACTOR=1` (inflate by nothing) and confirm it goes red.

### Why the canary's baseline is portable

Real baselines are absolute timings from `perf-runner-v1` and cannot run on a
hosted runner. The canary can, because its allocation count is fixed by its own
source (one `std::vector` construction per block — no container growth policy, no
stdlib version dependence), so inflating by N multiplies it by exactly N against a
10% tolerance. Its wall/CPU envelopes are set to 100000% on purpose: wall time in a
synthetic memset loop says nothing about the product, and gating it would only
manufacture cross-machine flakes. Keep it that way — the canary measures the
instrument, not the product.

## Mechanical Sweeps That Found Real Bugs

This tree is heavily reviewed, so reading files hunting for bugs has a poor hit
rate. These each found real defects, cost about one build, and are worth running
before any "find bugs everywhere" task.

**Extra GCC warnings** (scratch build dir, `-DMICROIDE_WERROR=OFF`):
`-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference
-Wshadow=local -Wsuggest-override -Wmissing-declarations`. Found TU-local helpers
with external linkage and dead `x==1 ? "a" : "a"` ternaries. `-Wlogical-op`
false-positives on `EAGAIN || EWOULDBLOCK` (same value on Linux). `-Wswitch-enum`
is pure noise here. `-Wduplicated-cond` and `-Wmissing-declarations` are now
permanent on the production targets via `MICROIDE_EXTRA_WARNINGS`.

**Build with the other compiler** (`-DCMAKE_CXX_COMPILER=clang++
-DMICROIDE_WARNINGS_AS_ERRORS=ON`). This found that the tree did not compile under
clang *at all*: a class nested in a still-incomplete enclosing class has its
NSDMIs unparsed, so clang evaluates `is_constructible_v<Nested>` as false while
instantiating the enclosing `std::optional<Nested>` and caches it — `opt.emplace()`
then fails, while GCC re-evaluates later and accepts. Fix was hoisting the nested
struct to namespace scope. Gate GCC-only flags on
`CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`: clang turns an unknown `-W` flag into an
error under `-Werror`, and the sanitizer presets use clang.

**Hardened stdlib.** `-D_GLIBCXX_ASSERTIONS` bounds-checks every container/string
access; `-D_GLIBCXX_DEBUG` additionally catches iterator invalidation and invalid
ranges. Both came back clean (a real confidence signal, cheap to repeat). Budget
~5× wall clock for `_GLIBCXX_DEBUG`; two shards will hit a 300 s ctest timeout,
and one such timeout was a safe-iterator artifact rather than a perf bug. Note the
debug run surfaced a genuine **load-sensitive test race** because the slowdown
changed thread interleaving — treat unexplained failures under a slow instrumented
build as timing bugs worth chasing, not instrumentation noise.

**Token-normalized clone detector.** A ~40-line script hashing sliding windows of
normalized non-trivial lines finds byte-identical blocks across files. Every hit
was a real drift risk, several security-relevant: duplicated subprocess sandbox
options, duplicated auth-session decoding, and four copies of a CLOEXEC pipe
helper where one had a fork race and two did not compile on macOS.

**N-of-N syscall-flag audit.** Enumerate every instance of a syscall family
(`socket|accept4|open|openat|pipe2|inotify_init1`) and check each for its
`*_CLOEXEC` flag. This found exactly one outlier in the whole tree —
`ControlSocketClient::Connect`, leaking the control-channel fd into every spawned
child. The shape generalizes: pick an invariant that should hold at *every*
instance, enumerate all instances, and look for the one that differs. An N-of-N
audit beats reading code because the outlier is self-evident. Now enforced by
`CheckDescriptorCreationIsCloseOnExec`.

**Test comments naming functions that do not exist.** Grep CamelCase identifiers
followed by `(` inside `//` comments in `tests/`, check each against the tree.
Found a test claiming to verify `ComputeIdleHint(...)` — a function that exists
nowhere — while actually asserting only that a counter increments. Fake coverage
reads as real coverage in a test list.

## Reachability Sweeps

Two mirror questions, both invisible to compilers, tests, and coverage — because
the tests are usually the only caller, which is exactly what makes the code look
healthy.

### "Handled but never produced"

*Enum values named only in `case` labels.* For each `Enum::Value`, count mentions
on lines that are not `case` labels. Zero producers means a branch nothing can
reach. Found four shipped-but-unreachable features, including
`WindowAction::ToggleFullscreen` (ran `SDL_SetWindowFullscreen`; no menu item,
command or keybinding produced it) and two `DebugBreakpointEdit*` modifiers
(hit-count breakpoints and logpoints implemented end to end, but the gutter menu
only offered "Set Condition…"). Now guarded for actions by
`CheckEveryActionIdIsReachable`.

*Struct fields whose identifier appears only at the declaration.* Found
`GitRepositoryState::operation_state` (written by nothing — the merge resolver's
rebase/cherry-pick label was unreachable) and LSP
`CompletionItem::sort_text_priority` (a dead int where `sortText` should have been
parsed, so completion used raw server array order).

*Declared vs consumed across a registry boundary.* "Hover Delay (ms)" and
"Scrollbar Size" were declared in `WorkspaceSettingsRegistry`, rendered in the
Settings overlay, persisted on change, and read by nothing — controls that
visibly lie. Now guarded by `CheckRegisteredSettingsAreRead`, the mirror of
`CheckSettingsReadAreRegistered`.

### "Implemented but never invoked"

Does anything in `src/` populate or call this? A store whose only writers are its
own tests cannot run in the product, however complete its read side looks.

Found: `ReviewCommentsRegistry` (deleted — unreachable, and its render lookup used
a filesystem path against a URI-keyed map); `TerminalSession::SendKey` (a second
key encoder whose only users were two tests, so those tests guarded a path the
shell never ran); `MarkFileUnreviewed`/`MarkHunkUnreviewed` (implemented and
tested, no ActionId — branch review could be marked and never undone).

The worst: `CommitWorkflowService::AcknowledgeWarning` was the sole writer of
`acknowledged_warning_ids`, and `CommitPreChecksAllowExecution` refuses a commit
on any unacknowledged Warning-severity pre-check. With no caller the set was
always empty, so `UntrackedFiles` / `BranchBehind` / `UnstagedLeftovers` behaved
as hard blocks — a repo with any untracked file could not be committed from
microide at all.

A third variant: **an accessor written for a test that no test calls.**
`TextBuffer::materialized_line_count()` existed solely so a test could pin "this
render path stays zero-copy" — and nothing called it, while the per-row render
path was in fact reading every line through the copying `operator[]`. Grep for
accessors whose comment says "so a test can assert …" and check they have a
caller. A guard nobody armed is worse than no guard: it reads as coverage.

### Running them

- Index every `identifier(` occurrence across `src/` **once**, then compare
  against out-of-line definitions `Class::Method(`. Per-symbol `rg` times out.
- Include `.inc` files — this repo puts shell members and TestAccess there.
- **False positives to expect:** functions passed *by reference*
  (`std::sort(v, LessProjectFile)`) have no call parens and look dead; also C
  callbacks handed to SDL/Lua, and `Class::Static(` forms. Confirm any finding by
  grepping the symbol by hand before acting.

### Triage matters more than the sweep

Three outcomes, and picking wrong does damage:

1. **Retired** → delete, and re-point any tests at the live path first.
2. **Unreachable but wanted** → wire it up (or file it if it needs a new API).
3. **Output taxonomy with live display branches but no detector** → leave it and
   file the gap. Deleting `MergeFileConflictKind::Submodule` or
   `PatchApplyResultCategory::StaleDiff` would silently downgrade what the user is
   told, not remove waste.

Check wire-format back-compat before deleting: `ProjectSessionTag::ChatRegistry`
looks dead but must stay, because old session files still carry the tag and the
decoder has to skip it. Deleting a *duplicate* is only safe after proving
equivalence — for the terminal key encoders that meant checking every enum case
emitted identical bytes for an unmodified press.

## Sanitizer Notes

- **TSAN mis-models `std::timed_mutex::try_lock_for`**, reporting a spurious
  "unlock of unlocked mutex". Use a plain `std::mutex` with a `try_lock` poll
  instead where a timed acquire is needed under TSAN.
- **TSAN needs ASLR relaxed**, but not `sudo`: `tools/run-checks.sh tsan` wraps
  ctest in `setarch -R` and exports the suppression file itself. The machine-wide
  `sudo sysctl vm.mmap_rnd_bits=28` is only a fallback if `personality()` is
  blocked.
- TSAN warnings do not fail the process — check the log, not just the exit code.

## Things That Break Silently Elsewhere

- **Bench and fuzz targets list explicit sources** rather than linking
  `microide_core`. Adding a dependency to a shared `.cpp` breaks their link — and
  fuzz targets break **silently**, because no default flow builds them. Smoke them
  with `tools/run-checks.sh fuzz --list`. Fuzz corpora contents are gitignored, so
  curated seeds need `git add -f`.
- **Adding a `WorkspaceCommandSpecs` entry** needs a README bullet and a
  regenerated man page (rebuild the `microide` binary first) or two doc-sync tests
  fail.
- **Doc paths go stale on file moves.** The same drift that hollows a lint hollows
  a doc. After a rename or subsystem move, scan docs for `src/…` references that
  no longer resolve. Dated investigation records are the exception — they describe
  the tree as it was.

## Related

- `guidelines/testing.md` — test strategy and the validation loop
- `dev-docs/performance/perf-harness.md` § Reading A Measurement — the
  measurement-side equivalents of these traps
- `AGENTS.md` § Do-Not-Regress Patterns
