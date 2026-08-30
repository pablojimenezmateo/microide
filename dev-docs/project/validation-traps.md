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

### `run-checks.sh` logs live at fixed paths, so a stale one reads as this run's result

`tools/run-checks.sh <lane>` writes to `/tmp/microide-<lane>.log`, deterministically
and without a timestamp in the name. That is the point — you can read a result back
without rerunning it — but the file **outlives the session that produced it**, and
nothing in its contents says when it was written.

Hit on 2026-08-06: a watcher tailing `/tmp/microide-{asan,ubsan,tsan}.log` reported
`100% tests passed, 0 tests failed out of 24` on all three lanes within seconds of
the sequence starting. The three lanes had not started — the sequence was still on a
prerequisite build. The logs were another session's, over an hour old, and the green
they carried was for a different tree. This is the previous trap one level up: there
the stale artifact was a binary, here it is the log *about* a binary, and neither
announces itself.

- **`rm -f /tmp/microide-<lane>.log` before starting the lane** if anything is going
  to read it programmatically. A missing file is unambiguous; a stale one is not.
- Otherwise check `ls -la` on the log against the wall clock before believing it,
  and confirm the lane actually ran (`run-checks: <lane> finished (exit N)` is the
  last line the wrapper writes).
- The same applies to `/tmp/microide-perf-*.log`, `--report-json` output paths, and
  any other fixed-path artifact reused across sessions.

### Two builds in the same directory clobber each other, and ctest says the binary is missing

`cmake --build build` is not safe to run twice concurrently against the same build
directory. The second invocation's link overwrites the first's output while it is
still being written, and the window where the file exists at zero bytes — or not at
all — is long enough for a `ctest` in the other pipeline to fail with
`Could not find executable .../microide_tests`, followed by twenty-four
`(Not Run)` lines.

Hit repeatedly on 2026-08-30. It is easy to walk into when builds are backgrounded
to keep working during them: a `run_in_background` build plus one started in the
foreground "just to check the errors" is two builds. The failure does not look like
a race — it looks like the build never produced a binary — and re-running it
usually succeeds, which makes it read as a transient.

Worse than the noise: the two builds interleave object files from *different source
states* if a source was edited between them, which is the same ODR-mismatch shape as
editing source mid-run (§ "Editing source while a build or sanitizer run is in flight").

- One build at a time, per build directory. Wait for it before starting another,
  and before starting `ctest`.
- If a build ends with a missing or zero-byte binary, check for a second builder
  (`pgrep -c cc1plus`) before believing the error.
- Separate build directories (`build/`, `build/microide-asan`, a worktree) are
  independent and may run in parallel; `tools/run-checks.sh` lanes each use their
  own, which is why they compose.

### Piping `run-checks.sh` into `tail` throws away the exit code that says whether it ran

`tools/run-checks.sh <lane> 2>&1 | tail -8` is the natural way to read a long lane's
result, and it is wrong: in a pipeline the shell reports **`tail`'s** status, which
is always 0. The wrapper's own exit code — the one that distinguishes "the lane ran
and passed" from "the lane never got past its build" — is discarded.

Hit on 2026-08-13. A TSAN run reported success while its *build* had failed on a
missing include: the last eight lines of a 323-step ninja build are eight
`Building CXX object` lines, so the tail looked exactly like a healthy run in
progress, and the harness that reported the command's status saw `tail`'s zero.
The failure was two lines further down (`ninja: build stopped`), and the wrapper had
written `run-checks: tsan finished (exit 1)` to the log.

- Redirect, then read: `tools/run-checks.sh tsan > /tmp/out 2>&1; echo $?`.
- Or grep the log for the wrapper's own last line, `run-checks: <lane> finished
  (exit N)`, and believe the N — never the tail's shape.
- `set -o pipefail` fixes it in a script; it is not on in an interactive shell.

### The documented inner-loop build is Release, so `#ifndef NDEBUG` tests never run in it

`cmake -S . -B build` produces `CMAKE_BUILD_TYPE=Release`, and a family of tests
is compiled out there: every counter behind `#ifndef NDEBUG`
(`WrappedRowLayoutBuildCountForDebug`, the incremental splice/in-place counters,
the visual-column ones) and every `AddTest` registration guarded by the same.
`microide_tests` prints a smaller test count in `build/` than in a sanitizer build
and says nothing about the difference.

Hit on 2026-08-12: a change to `SetViewportSize` broke
`SoftWrapViewportResizeRebuildsWrapCacheLazily`, and three full green
`./build/microide/microide_tests` runs (2,819 passed) did not notice, because that
test does not exist in a Release build. The ASAN lane — which is Debug — failed it
on the first try, at 2,825 tests.

- A green Release run is not evidence for anything a debug-only counter asserts.
  Before concluding, run the same change through a Debug build: either a
  sanitizer lane, or `./build/microide-asan/microide/microide_tests <filter>`.
- The test-count difference between the two builds is the tell. If it is not the
  number you expect, some assertions are compiled out.

### A type trait can answer differently in the two compilers, so one lane rejects code five lanes take

`clang-build` is not just a warnings lane. It is the only place in the repo that
compiles the tree with a second front end, and front ends disagree about more than
diagnostics — they disagree about **type traits**, which are load-bearing for
overload resolution in the standard library.

Hit on 2026-08-14 merging the tab-drag branch. `TabEntry` declares
`std::optional<EditorTabState> editor_state;` as a member of the class
`EditorTabState` is nested in. Clang evaluates `__is_constructible(EditorTabState)`
at that declaration, while `TabEntry` is still incomplete, and caches **false**
for the rest of the TU; GCC re-evaluates later and answers true. So
`optional<EditorTabState>::emplace()` — which is constrained on exactly that trait
— has no viable overload under clang and compiles fine under GCC. A test using it
passed `tests`, `perf-tests` and every sanitizer lane, and `clang-build` was the
single thing that failed. See [TD-2026-08-14-214](known-tech-debt.md).

- The failure does not look like a portability problem from the GCC side; it looks
  like working code. There is no warning to escalate and no runtime symptom,
  because the translation unit never builds at all under the other compiler.
- Reducing it is worth the five minutes: a `static_assert` on the trait, plus the
  same struct copied out to namespace scope, separates "my type is broken" from
  "my type is fine and the trait is context-dependent". Here the copy was
  constructible and the nested original was not, which is the whole answer.
- `T a;` compiling is not evidence that `is_constructible_v<T>` is true. They are
  different questions to a compiler that has cached one of them.
- Run `clang-build` before a merge, not only after adding a source to a curated
  target list. It is the cheapest lane that can reject code nothing else rejects.

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

### A test that only fails under TSAN is a product race until proven otherwise

`WorkspaceShell/ReplaceAllFallsBackWhenResultsTruncated` went red on the tsan
lane in CI four times across 2026-08-03/04. It was diagnosed twice as a test
problem — once "a TSAN race", once "the index-size wait is wrong" — and twice
the fix was a better wait in the test. Both times the message it printed got
sharper, and the third sharpening is what identified the actual defect: project
search stopped claiming candidate files the moment the result cap filled, but
only flagged `truncated` when a worker *attempted* a match past the cap. Fill the
cap exactly and no worker ever attempts one, so files went unscanned while the
sidebar reported a complete result set. TSAN was not perturbing the test; its
slower scheduling was just making a real product race reproducible.

The tell was in the failure text, because a previous round had put the numbers
there: `candidates=210, results=200`. The precondition (all 210 files reached the
search) had already passed, so "the test did not wait long enough" was
excluded by the message itself.

- A sanitizer lane is a scheduler, not a mutation. Anything it makes fail is
  reachable in production on a loaded machine.
- Before adding a wait, make the assertion **name the numbers it compared**. Two
  rounds of that here converted an unfalsifiable "must mark the results
  truncated" into a defect report. It is also what makes a single CI occurrence
  actionable without a local repro — see the sibling
  `FilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild`, which now samples
  the finder's state *before* the Enter that destroys it.
- A regression test for a boundary race usually has to *remove* concurrency to
  be honest. The one added here passed five for five against the un-fixed
  service at full worker count — several workers are mid-file when the cap
  fills, one of them attempts an over-cap match, and the old code path fires.
  Pinning `MICROIDE_SEARCH_WORKER_LIMIT=1` is what exposes the exact-fill
  boundary with nothing left to mask it.

### A lane that runs a test binary directly gets none of ctest's fixture setup

The `perf-canary` lane runs `microide_perf --scenarios=perf_gate_canary`
straight from `run-checks.sh`. The large `editor_essentials_*` fixture trees are
gitignored and generated by the ctest setup test `microide_perf_fixtures`, which
that lane never runs — so on a clean CI checkout the trees were absent, PerfMain
hashed them anyway, and reported the empty-tree digest
(`e3b0c442…b855` — SHA-256 of nothing, worth recognizing on sight) as a fixture
**corruption** mismatch. Red for five consecutive runs on a lane that reads no
fixture at all.

- `FIXTURES_SETUP`/`FIXTURES_REQUIRED` only bind inside ctest. Any lane invoking
  the binary itself must either do the setup or not need it.
- Startup-time global validation costs every invocation, including the ones that
  touch none of what it validates. Check what the selected work actually uses.
- One "the fixture is not here" policy per binary. Two — a hard failure at
  startup and a graceful skip per scenario — is how the disagreement stayed
  invisible until a lane hit only the strict one.

### A lane that narrows the build target but not the test selection

The inverse of the trap above, and it shipped in the same script. `run-checks.sh
tests` built `--target microide_tests` for inner-loop speed and then ran the
whole of `ctest` — which also invokes `microide_perf --smoke`. That binary was
therefore whatever the last full build produced, and nothing said so:

- The loud direction: a perf scenario registered after the last `microide_perf`
  build makes `FindStaleBaselineScenarios` report 13 committed baselines as
  belonging to unregistered scenarios. The message describes a source problem
  that does not exist. Found 2026-08-10, and it cost a diagnosis.
- The quiet direction is the one that matters: any change to the perf harness or
  to product code the smoke run exercises is validated against the previous
  build, and the lane reports green.

If a lane scopes the build, it must scope the test selection to match, or build
every target the selection invokes. Fixed by building `microide_tests
microide_perf` — the two binaries ctest actually runs in the default build.

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

### A lint whose cost is (patterns x files) is one busy machine from the watchdog

The architecture lint reads the whole tree with `std::regex`. Two of its rules
did it once per *pattern* rather than once per file, and the cost compounds
invisibly: `CheckCoordinatorOperationsAreCalled` compiled a regex per
`Operations` field and ran it over that field's whole include scope — ~200
fields x ~1,000 files. 3.3 s natively, and **326 s under TSAN with six ctest
shards on four cores**, which is past `microide_tests`' 300 s per-test watchdog.
The lane went red with zero ThreadSanitizer warnings, so the failure said
"Subprocess aborted" and nothing about why.

Two rules to take from it:

- **A slow lint is a flaky lint.** A per-test timeout turns "slow" into "fails
  under load", and load is exactly what a sanitizer lane has. Measure a new rule
  natively and assume ~100x under TSAN with contention: anything over ~2 s
  natively is already inside the failure envelope.
- **Sharding is not the fix; the scan is.** TD-2026-08-10-171 first split
  aggregate rule tests into one ctest case per rule, which was right and did not
  help here — the slow thing was one rule. Collect what every file says once and
  query it per pattern, rather than asking each pattern of every file.

When you do rewrite a rule's scan, the evidence that it still works is its
positive **and** negative fixtures (`ArchitectureInvariants/TargetedScannerFixtures`),
not a green run of the rule itself: a rule that now matches nothing also passes.

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

### A gate whose verdict depends on `--iterations`

A scenario whose first iterations cost more than its steady state does not have a
noisy gate — it has a gate that answers a *different question* at each iteration
count, because p50/p95 are order statistics over however many samples you took.
Two of them were caught this way on 2026-08-04, both failing at `--iterations=8`
and passing at `--iterations=20` from the same binary:

| scenario | cold shape | metric that flipped |
| --- | --- | --- |
| `repo_open_rss_idle` | iteration 0 = 3,685 allocations, steady state 550 | `p95_allocations` +55% |
| `merge_next_conflict_large_file` | settles over ~7 iterations (39,069 → 11,27x → 7,779 allocations; RSS growth 9.3 MB → 0) | `p50_rss_growth_bytes` +2259% |

Neither was a code regression, and neither was noise: allocation counts here are
deterministic to the unit across runs. The fix is `warmup_iterations`, which the
harness already supports and which several scenarios already carry with comments
saying exactly this.

**How to tell them apart from a real regression**, since both present as a red
gate: print the per-iteration series, not the summary. `--report-json` carries
every iteration's `allocations`, `rss_growth_bytes` and `wall_ms`. A regression
moves the *whole* series; a missing warmup leaves a steady tail sitting at or
below the committed baseline with a head that does not. If the tail is clean, the
scenario needs a warmup, not a fix — and rebaselining it instead would bake the
cold pass into the number and hide the next real regression underneath it.

#### The mirror: a settling statistic gated at a short count

The entry above is about a scenario that needs a warmup. This is about a metric
that needs a sample size, and it produced a **476 % phantom regression** on an
unchanged binary.

`p50_net_heap_bytes` is documented as deterministic to the byte, and it is — *at
a fixed iteration count*, which is not the same claim.
`editor_indent_guides_paint`, one binary, ten iterations:

```
12,286,512  341,770  297,236  63,389  -7,535  29,836  48,084  -20,348  50,584  25,080
```

p50 over 3 = 341,770. Over 5 = 297,236. Over 10 = 49,334, against a 59,735
baseline. Five of twelve editor scenarios were red at `--iterations=5` and green
at 10, and the failure line named the metric, the percentage and the envelope —
everything except the sample size (TD-2026-08-10-173).

Two rules come out of it:

- **Before gating a statistic, ask whether it converges.** `mean_rss_growth_bytes`
  and `p50_net_heap_bytes` are both statistics over a settling series, and both
  had to learn separately (TD-2026-08-06-148, then TD-2026-08-10-173) to decline
  a comparison against a baseline recorded over more iterations. Durations carry
  machine state and are already annotated.
- **"Allocation percentiles are safe by construction" was wrong, and this
  document said it.** An allocation *count* is deterministic; a *percentile over
  a settling series of them* is not. Twelve phase `p50_allocations` gates fail at
  `--iterations=2` and pass at 10 on an unchanged binary (TD-2026-08-10-181) —
  including `merge_large.open_to_first_paint`, whose 185-allocation baseline is
  the *re-open* an already-open merge tab costs, while the open it is named for
  costs 27,293 and happens only on iteration 0. Phase allocation gates carry no
  "NOT ENFORCED at a short count" guard because they are believed exact, and the
  tracer workflow this repo documents (`perf-harness.md` § Finding *where* a
  phase allocates) tells you to run `--iterations=2`. So the documented debugging
  loop produces a dozen red gates and nothing explains them.
- **A phase gate whose baseline is far BELOW its own first iteration is naming an
  operation it does not run.** That is the `merge_*.open_to_first_paint` shape:
  the scenario re-runs its open command per iteration against a driver that
  reuses the already-open tab. Compare a phase's committed `p50_allocations`
  against its `max_allocations` in the same baseline — a two-orders-of-magnitude
  gap is that tell, and it is visible without running anything.
- **A red gate on a short run is a suspect, not a result.** Both metrics now say
  `NOT ENFORCED: this run measured 5 iterations against a baseline recorded over
  10`. Read the note before starting a bisect.

### A scenario can regress 1.9x from a change that cannot reach its code

`reference_snippet_file_window` — which writes a temp file and calls
`util::ReadFileLineWindow`, touching no editor and no subsystem the change went
near — went from 35 ms to 65 ms across the 2026-08-04 fold rewrite. Rock solid:
four runs of each binary spread 34.5–35.8 and 64.9–65.6, order-independent, same
allocation count to the unit, CPU time equal to wall.

It was code alignment. Check this **before** believing any cross-binary wall
delta:

1. `md5sum` the object file of the code that got slower in both builds. Here
   `TextFileIO.cpp.o` was byte-identical — same source, same flags, no LTO — so
   the machine code was not what changed.
2. `nm -C <binary> | grep <symbol>` in both. The function sat at a 64-byte
   boundary in one build and 32 bytes into a cache line in the other, because an
   unrelated translation unit ahead of it in link order had changed size.
3. Rebuild with `-falign-functions=64 -falign-loops=32`. The "slow" source then
   ran the scenario at **32.8 ms** — faster than the "fast" binary — which
   confirms layout rather than work.

Step 3 is a diagnostic, not a fix, and it was measured rather than assumed:
building the whole suite with those flags is a **wash** — mean −0.6%, median
−0.5% across 93 scenarios, with individual swings from −21% to +10% in both
directions. Alignment moves this suite around by ±20% at random; it does not make
it faster.

The moral is that **a wall delta on a scenario the change cannot reach is not
evidence of a regression**, and the three checks above settle it in minutes.
Allocation counts do not have this failure mode, which is the reason this suite
gates primarily on them.

### A green suite proves nothing about a shape no fixture has

The gate can be perfectly healthy and still be blind, because coverage is a
property of the *fixtures*, not of the gate. On 2026-08-05 the whole 95-scenario
suite was green while a one-character insert allocated **13x the affected line's
bytes** — five redundant whole-line copies per keystroke on the editor's single
hottest path.

Nothing was broken about the instrument. Every editor fixture in the tree is
ordinary line-broken text (the widest line in `large_project` is 20 bytes), so
no scenario ever put a long line through the edit path, and a cost that is
proportional to line length is a rounding error on 20 bytes. The same code on a
minified bundle — one line of megabytes, which the editor opens with no size
guard — was ~22 ms per keystroke.

The generalisation is worth more than the instance: **for any cost that scales
with some input dimension, ask which fixture spans that dimension.** Line length
had no fixture. Others to check before assuming they are covered: a file with
one enormous line (now `editor_essentials_minified`), a file with hundreds of
thousands of very short lines, a line with dense non-ASCII, a project with one
directory of 50k files.

Two habits made the instance findable in about an hour, both already in the tree:

- `MICROIDE_PERF_BIG_ALLOC_BYTES=<n>` prints a backtrace for every allocation at
  or above `n` (`tests/perf/AllocationCounter.cpp`). Resolve the frames with
  `addr2line -e <binary> -f -C <offset>`. It named all five sites directly;
  no profiler is needed and none works on this host anyway.
- `MICROIDE_PERF_ALLOC_TRACE=<min>[:<max>]` is its complement, and the one to
  reach for when the problem is *many small* allocations rather than a few large
  ones. It captures a stack for every allocation in the size **band** and
  **aggregates** by stack, printing the table most-frequent-first at the end of the
  run. One backtrace per hit is unreadable at 960 hits, and a floor cannot express
  "exactly 32 bytes". TD-2026-08-06-139 needed a 90-commit bisect for want of this;
  after it, `MICROIDE_PERF_ALLOC_TRACE=32:32` named the site in one run. Resolve
  with `addr2line -e <binary> -f -C -p -i <offset>...` — `-i` matters, because
  everything interesting is inlined.
- `MICROIDE_PERF_ALLOC_TRACE_PHASE=<substring>` scopes that table to the measured
  phase, and you almost always want it. The band filter narrows by size; this
  narrows by **when**. Without it the table is whole-*run*, and a scenario's setup
  out-allocates its measured phase by an order of magnitude: chasing the
  allocations left in `editor_mouse_selection_drag` after TD-2026-08-06-145, the
  printed top twelve were all syntax-registry and plugin-reload sites the phase
  never executes, and the phase's own two sites did not appear at all. The trap is
  that the table looks like it answered. With the filter set,
  `ScenarioContext::Measure` arms recording only inside matching phases and the
  same run named both sites from four rows. A filter that matches no phase says so
  loudly rather than printing an empty table, which would read exactly like "the
  phase allocates nothing".
- `MICROIDE_PERF_SUMMARY=1` ranks `PerformanceTrace::Scope` regions by self time.
  The edit path carried **no scopes at all** — `BuildRangeHistoryEntry`,
  `ApplyHistoryEntry` and the buffer write were unmeasurable — so the first step
  was adding them. A hot path with no scopes is itself a finding.

And when you close the gap, **gate on bytes or counts, not duration**, and check
the tolerance is tight enough to catch what you just fixed: the default 10%
allocation envelope would have passed the +7.7% regression that
`editor_typing_minified_line` exists to catch, so it runs at 1%. Then reintroduce
one fix's worth of the regression and confirm the gate goes red — it did, at
+1.93%.

### A differential test whose control shares the code path is only a consistency check

On 2026-08-05, three row match-fill loops were changed to narrow their span list by
the row's visible columns instead of only by its line. The risk of any narrowing is
the opposite failure — dropping something that should have been drawn — so the
change came with a pixel differential: render the scrolled row, render the same
bytes unscrolled as a control, require the two identical.

**Three of four deliberately injected bound bugs passed it.** The control goes
through the same production narrowing, so a bound that is wrong the *same way on
both sides* loses the same fills in both renders and the two still match. The
differential catches asymmetric bugs and is structurally blind to symmetric ones —
which is the class an off-by-one or a too-tight bound most often produces.

The second attempt was no better: count pixels painted in the highlight color.
Highlight fills are alpha-blended and then overpainted by glyphs, so the color
never appears literally and the count is always zero.

What works is an oracle that does not touch the code under test: compute each
in-window match's cell position from `ComputeMetrics` and the char width, then
assert each one changed pixels against a render of the same frame with no matches
at all. All six injected bound bugs fail that.

Generalising: **when a change makes something narrower, the reference has to be
something that could not have been narrowed.** A second run of the same code with
different inputs is not one. Ask what would still be true if the new bound were
wrong in both directions at once — if the test would still pass, it is measuring
self-consistency, not correctness.

The same run is also the source of the sibling rule in
`### A bound in lines is not a bound on work` below.

### A bound in lines is not a bound on work

Two independent stalls in the same 2026-08-05 sweep were both "we already bounded
this", with the bound in the wrong unit:

- `FindBracketMatch` bounds its scan by `max_lines_each_side` (2000). On a file
  with no line breaks in it, two thousand lines is the whole document, so one
  arrow key next to the closing bracket read megabytes — 1.7 ms per caret move.
- The bracket matchers' string/comment suppression is a per-LINE question asked
  per COLUMN, so a scan cost one highlight-cache probe per BYTE: 12,584,364 of
  them across 40 frames.

A bound expressed in lines, rows, entries or elements bounds work only if those
units have bounded size. When the unit is user-controlled — a line, a match, a
directory entry, a terminal row — state the bound in bytes as well, and size it
against the widest realistic input rather than a round number
(`kMaxBracketMatchScanBytes` is 512 KiB because the 50k-line C++ fixture's full
2000-line window measures ~326 KB).

### A committed baseline is not a proxy for main

Running the full perf gate after a change showed several scenarios comfortably
under their committed baselines, which reads as "this change improved them". An
interleaved A/B against the base commit — same preset build, same lane — showed
they measure **identically on both sides**: `editor_shaping_multi_caret` records
103,880 allocations and measures 57,362 on base *and* HEAD, and three more
scenarios are 40-80% loose the same way (TD-2026-08-05-135).

So a gate can drift loose without anyone noticing, and the drift then reads as
someone else's win. **A/B against the base commit before attributing any delta to
your change**, and rebaseline only what your change actually moved — folding
pre-existing drift into your numbers makes both unreadable.

The 2026-08-06 sweep that closed that entry found eleven allocation gates and six
wall gates carrying drift, and — the part worth carrying forward — **five gates
that had drifted the other way**, up to +9.4% allocations, all of them passing.
A sweep that rewrites every number with whatever the machine says today closes the
loose gates and quietly enshrines the tight ones' regressions. The rule:
**rebaseline down, investigate up.** A scenario measuring under its baseline is
drift and may be tightened without ceremony; a scenario measuring over it is a
finding, even when it is inside its envelope, and gets looked at before its number
is written. Put the drift table in the commit message either way — it is the only
record of which direction each gate moved.

### A baseline recorded at the wrong iteration count cannot be held

`--update-baseline --iterations=20` recorded `typing_small_file`'s
`p95_allocations` as 525.9. A default-length gate run failed it immediately at
2,194. The scenario has a ~3,700-allocation cold first iteration, so at ten
iterations (five samples) the p95 lands on the cold pass and at twenty it does not
— the same iteration-count sensitivity documented above for verdicts, now on the
recording side.

**Record a baseline the way the gate runs it**: `--update-baseline
--reference-runner=perf-runner-v1`, no `--iterations`.

### A bound that reads its input before rejecting it

The next level down from the above, and it is easy to miss precisely *because* the
bound is there and correct. Four separate long-line caps in the editor were applied
after the line had already been read:

```cpp
void ScanLine(std::string_view line, std::vector<CachedBracket>& out) const {
  if (line.size() > kMaxBracketScanLineBytes) return;   // correct, and too late
  ...
}
// caller:  table.ScanLine(lines[i], out);   // `lines[i]` copies a piece-tree line
```

So the very lines the cap exists to skip were the expensive ones — a
multi-megabyte copy per keystroke, discarded on the next statement
(TD-2026-08-05-133). `SignatureDetectHead` spelled it `lines[i].substr(0, 4096)`,
which *reads* bounded and *asks* unbounded.

**Ask the LENGTH, not the bytes.** `LineLength` is two offset lookups and reads no
text. Where a caller needs part of a line, `LineWindow(line, start, len, scratch)`
asks for exactly that. A `substr` on a whole-line accessor is the tell.

### A "fragmented" fixture that is not fragmented

Three tests written in that same pass made a piece-tree line span pieces with a
**zero-length splice** (`ReplaceTextRange(0, mid, 0, mid, "")`). That is a no-op:
the line stayed contiguous, the copying path each test named was never taken, and
every test passed. Insert a byte and delete it again — the tree never re-merges
pieces — and then assert the fixture actually materializes before relying on it.
This is the architecture-lint vacuity problem in fixture form: a fixture that
cannot reach the path it names is worth nothing, and it looks exactly like one that
can.

### A differential whose oracle is insensitive to the bug

Stronger than the "control shares the code path" trap above, because here the
oracle is genuinely independent — and still blind. The chunked indent scan was
tested against `FoldingReference.h`'s own single-pass measurement, on a document
whose lines all shared one indent string. Fold output only compares indents
*against each other*, so a consistently wrong measurement produces identical folds:
the test passed against a deliberately broken chunk carry.

**Check what the output is sensitive to, not just where the oracle comes from.**
The fix was a fixture where two lines land one column apart across the chunk
boundary, with a tab size that does not divide the chunk (so the boundary is not a
tab stop) — then a wrong carry changes the fold *tree*, which the output can see.

### A gate that measures the machine, because every metric it holds is a duration

`idle_soak_30s` failed the perf gate at `p50_cpu_ms +103%` — and passed the same
gate, same binary, on the runs either side of it. Wall, allocations, RSS and its
zero-wake assertion were all green, and **every application perf counter was
byte-identical between a 14 ms iteration and a 30 ms one**.

`cpu_ms` and `wall_ms` are durations, so both scale with the machine's effective
clock, and nothing in the report said what that clock was. `harness.cpu_calibration_ns`
now does: a fixed 400k-step dependent integer chain, timed just outside each
iteration's window so it is charged to neither metric. On the reproducing run it
stepped 671 → 857 us at exactly the iteration where `cpu_ms` stepped 14 → 30 ms.
The governor had walked the core down; nothing about the binary changed.

**Two things generalise:**

- **A scenario that sleeps is a scenario that lets the clock drop.** 27 of this
  scenario's 30 seconds are sleep, which parks the core at the 605 MHz floor —
  8.5x below its 5157 MHz ceiling. What is left to measure is a handful of frames
  rendered on the way back up. `repo_open_rss_idle` shows the same signature: its
  calibration swings 671–1352 us *between iterations of a single run*.
- **Check what the gated number is actually made of before rebaselining it.** This
  scenario's whole ~15 ms CPU budget was 18 *harness* frames at ~0.83 ms each; the
  idling it exists to measure was the ~2 ms residual underneath. No baseline and no
  envelope could fix that — the fix was to measure CPU across the soak window
  directly and assert it, which is stable at 3.85–10.69 ms across the same clock
  step that doubled the iteration number.

Ruled out along the way, each cheaply and each worth ruling out before reaching for
a thermal or scheduler story: the process was already pinned to the 8 fastest CPUs
(`--pin-cores=auto`), so it was not E-core placement; calibration over a 40-minute
gate run came out at 0.97x first-10 vs last-10 with temperature ending where it
started, so there is no thermal drift; a 10-scenario warm-process prefix reproduced
the baseline p50 exactly (14.69 vs 14.6955), so it was not cache or RSS pressure
from earlier scenarios; and the harness's own idle-poll count swings 530–5,606 per
iteration (10.6x) while moving CPU by 15%, so it was not the poll loop.

**Habit:** when a perf failure is CPU-or-wall-only, with allocations and
application counters unchanged, read `harness.cpu_calibration_ns` before reading
the diff.

### An "exactly deterministic" metric is only deterministic in what the harness controls

`repo_open_rss_idle` read **369 allocations** against its committed baseline of 369
on a machine that had never run microide, and **627** on the workstation the
baseline was recorded on. Both figures reproduced to the byte across runs, on
identical code. Allocation counts are the suite's oracle precisely because they do
not move — so a stable wrong number is worse than a noisy one, because nothing
about it looks like noise.

The harness does isolate: `EstablishIsolatedAppRoot` points
XDG_CONFIG/STATE/CACHE/DATA_HOME at a fresh `/tmp` tree, and there is a test
pinning that it exports all four. What it did not control was **when**.
`PerfHarness::Driver` holds a `workspace::WorkspaceShell` **by value**, so
`Driver driver;` runs the shell's constructor — which resolves the user state root
and loads `recents` from it — and the isolation was established one statement
later, inside `InitializeDriver`. Writes landed in the isolated root, so the
sandbox always looked right; only the reads leaked.

`strace -f -e trace=openat` settled it in one run: the scenario child's first
`openat` is `/home/<user>/.local/state/microide/recents`, and the next one is the
isolated root being created.

**What generalises:**

- **Ask what the measured code reads, not just what it writes.** A sandbox
  verified by "the artifacts are in the right place" is verified in the one
  direction that cannot fail here.
- **A by-value member is a constructor call at its declaration.** Any setup that
  must precede the object cannot live inside a function that takes it by pointer.
- **Reproducibility is not correctness.** "It measures the same number every time"
  and "it measures the thing it names" are independent claims; this metric had the
  first and not the second for as long as the harness has existed.
- The ordering now carries an architecture lint
  (`CheckPerfHarnessIsolatesBeforeConstructingTheShell`) with negative, positive,
  comment-masking and missing-anchor controls, plus a process-level isolation floor
  in `PerfMain` so a future mis-ordering degrades to a shared `/tmp` root instead
  of the operator's home directory. See TD-2026-08-07-165.

### A measured phase whose body is a wall-clock wait gates the runner, not the code

`linter_on_save`'s gated phase was:

```cpp
context.Measure("linter.wait_diagnostics", [&]() {
  (void)context.WaitForDiagnostics(source, std::chrono::milliseconds(120));
});
```

`WaitForDiagnostics` polls to a deadline and pumps a frame per iteration. Its
allocation count is therefore *how many times a wall-clock loop got round* — a
duration wearing an allocation's clothes, gated at the 10 % tolerance the suite
reserves for the metric whose entire justification is that it is byte-identical
run to run. One unchanged binary read **3,561 / 3,644 / 3,711** against a
committed baseline of 2,745 recorded on an idler machine.

Underneath that sat the larger fact: **the wait had never once succeeded.**
Nothing in this repository lints JavaScript — no linter service, no config in the
fixture, an empty `node_modules/` — so every run since the scenario was written
spun the full 120 ms, found nothing, and reported the poll loop as the result.
The `(void)` cast is what let it: a wait whose success is load-bearing must
`SkipScenario()` on failure, not be discarded.

**What generalises:**

- **Grep for it**: `rg -n 'Measure\(' -A4 tests/perf/*.cpp | rg 'WaitFor'`. Every
  hit is a candidate. `WaitForFileIndexPath` and `WaitForProjectSearchFinished`
  have the same shape and are unaudited (TD-2026-08-10-180).
- **The test for it is three runs, not one.** A phase whose `p50_allocations`
  moves by more than a few between runs of an unchanged binary is timing the
  runner. This is cheap and nobody was doing it.
- **Raising a timeout is a diagnostic.** Going 120 ms → 500 ms turned the pass
  into a SKIP, which is what finally said the wait had always failed. A phase that
  gets *cheaper* when you give it more time is measuring the timeout.
- **The fix is to drive the path, not to wait for it.** Publish the diagnostics
  through the store exactly as an LSP publish would, then measure the fixed number
  of frames that apply and paint them. Bump `measurement_revision`: the old and
  new numbers are not comparable (TD-2026-08-07-167).

See TD-2026-08-10-179.

### A lint whose regex was never run against a known positive is not a lint

`CheckFactoryResultsAreNotCapturedByValue` shipped in two broken drafts before the
one that works, and only one of them was caught by its own fixtures.

The first spelled the capture-list scan `\[[^]\n]*\bNAME\s*[,\]]`. ECMAScript
reads a **leading `[^]` as "any character"** — the `]` does not close the class —
so the pattern matched essentially everything, and the rule reported green on the
exact defect it was written for. Its negative-control fixture "passed" too, for the
same reason, so the fixture suite was green and meaningless. What caught it was
reintroducing the original defect *in the real tree* and observing that the rule
stayed silent.

The second flagged `[&name]`. A reference capture is free; the rule is about
by-value ones, and the distinguishing information is entirely in the surrounding
characters. The pattern now anchors on a capture-list opener or separator
(`[\[,]\s*NAME\s*[,\]]`) rather than scanning the interior. That one *was*
caught by a fixture — the third control, written specifically because a reference
capture is the obvious false positive.

**What generalises:**

- **Two verifications, not one.** Fixtures prove the rule's *logic* on synthetic
  input. Reintroducing the real defect in the real tree proves the rule's
  *pattern* against real source. A regex bug can pass the first and fail the
  second, because the same bug is present in both.
- **Write a fixture for the obvious false positive**, not just for the defect and
  the fix. `[&x]` vs `[x]`, a comment mention, a same-named unrelated local. This
  is where an over-broad pattern shows up.
- Repo precedent for the same lesson: `CheckDescriptorCreationIsCloseOnExec`
  shipped with `openat?` as its `open` pattern — which matches `opena`/`openat`
  and never plain `open(` — and passed green while blind.

See TD-2026-08-10-177.

### A tracer whose site table fills prints a ranking of the sites that got there first

The allocation tracer aggregates by call stack into a fixed open-addressed table.
When that table is full it drops each **new** site whole rather than merging it
anywhere — so the "most frequent first" listing it prints covers only the sites
seen before it filled, and the phase's largest site can be missing from it
entirely.

It was. `switch_and_idle.switch_and_settle` reported 6,188 allocations from 1,024
sites with 17,807 dropped (74 % of the phase). After enlarging the table the true
#1 is 720 allocations and the old table's "#1" — 192 — is #4.

**What generalises:** an aggregating instrument with a bounded key space has two
failure modes, and only one of them looks like a failure. Merging on collision
degrades gracefully and visibly (one row is too big). Dropping on overflow
produces a table that is internally consistent, correctly sorted, and wrong. If a
tool prints a top-N, check whether its N-selection saw everything — and make the
overflow message state the *consequence* ("this ranking is not a ranking"), not
the remedy ("raise kTraceBuckets"), because the remedy reads as an optimisation
note and gets skipped.

See TD-2026-08-10-178.

### A gate can be MINTED for a metric nobody measured

`SaveBaseline` guards cpu, calibration and net-heap behind their `has_*` flags —
a record that never measured them writes no field, and `LoadBaseline` reads the
absence as "not gated". The four resident fields were written unconditionally,
including as zeros. So an `--update-baseline=deterministic` over a baseline that
predated the resident metric — a mode whose entire contract is "I am not entitled
to take a machine-sensitive reading" — wrote `mean_rss_growth_bytes: 0`, and the
next load called that a recording and gated on it with only the 64 KiB floor for
slack.

The victim (`editor_moby_dick_workout`) then failed `baseline=65536
measured=1.57e6` for a month, and the failure read as a leak. It was a gate for a
number the file never contained.

**What generalises:** presence-keyed optionality has to be enforced on the WRITE
side as well as the read side. If a loader infers "not recorded" from a missing
field, every writer that emits a default value is quietly converting "unknown"
into "measured zero" — and zero is the tightest gate in the suite.

See TD-2026-08-15-250.

### A scenario can gate a path its own fixture prevents from running

`external_change_refresh_open_merge` gated 31 allocations for six pumped frames
around a refresh that never happened: its fixture primed the project by
**assigning** `current_project_state.root` rather than opening it, so there was no
file index and no watcher for the change to be observed through, and the forced
check returned false immediately. The gate was green, stable, and measuring a
no-op — the most durable kind of vacuity, because nothing about a no-op is noisy.

It surfaced only when an unrelated change (retiring the second project watcher)
moved the forced path's cost, which moved BOTH external-change scenarios: one down
2,073 -> 892, which would have been read as a win.

**What generalises:** a cheap fixture prime is right until the thing it skips is
the scenario's subject. When a scenario is named for a subsystem's response,
check the run's `perf_counters` block for evidence that the subsystem actually
ran — `watch.file_index_apply_batch_calls`, `compare.model_builds` — rather than
trusting the phase name. A phase whose counters are all frame plumbing is not
measuring what it says.

See TD-2026-08-15-253.

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

**Count the syscalls, then ask which line makes them.** `strace -f -c -e
trace=newfstatat,statx,getdents64,openat` over a 4-second launch is one command
and answers "is this CPU or is this the kernel" without a profiler (this box has
no unprivileged `perf`). Launch on this repo read **103,064 `newfstatat` calls**
for ~55,000 filesystem entries — an obviously wrong ratio, and the second
`strace` (without `-c`, bucketing the paths) said which subtree. The cause
generalizes well beyond this tree: `std::filesystem::directory_entry::status()`
always calls the free function, one stat per entry, while `is_directory()` /
`is_regular_file()` answer from the `d_type` `readdir` already returned. A
20-line standalone program comparing the two over the same tree measured 712 ms
/ 50,844 syscalls against 127 ms / zero — write that program rather than reading
libstdc++, it takes two minutes and the answer is unambiguous. See
`platform::EntryPathType`.

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

## The Test Environment's Default Configuration

A bug that only exists off the configuration the tests run in is not caught by
any number of green runs, and the two found on 2026-08-17 were both this shape.
The whole suite drives SDL under `SDL_VIDEODRIVER=dummy`, which reports a display
scale of exactly **1.0**; the perf harness does the same; and every launch sweep
before that one was run on a machine whose user config held nothing but defaults.

- **`SceneTexturePresenter::Ensure` compared a LOGICAL size against
  `SDL_GetRenderOutputSize`, which is PIXELS**, and gave up when they differed.
  They differ whenever the display scale or the UI scale is not 1.0 — so on every
  HiDPI desktop the retained scene texture was never created, every frame took
  the direct-to-window fallback, and the entire partial-redraw path was dead
  code. Under `dummy` the two numbers are equal, so the suite only ever exercised
  the case where the bug cannot appear. See TD-2026-08-17-254.
- **The first prepared frame re-applied `project.files_exclude`** because the
  last-applied memo it compares against started empty — which is also the
  setting's default, so with a default config the comparison happens to be
  correct. Set the setting and every launch pays a whole extra tree walk. It was
  found only because an unrelated A/B had left the setting in the developer's own
  config. See TD-2026-08-17-255.

What follows from it:

- when a value has a "same as default" initial state, ask what happens when the
  restored value is NOT the default — that is a different code path, and it is
  the one real users are on;
- a headless driver's answers (display scale, renderer name, output size,
  refresh rate) are that driver's answers, not the platform's. A number or a
  branch that depends on one has not been tested by the suite;
- when a fix lands for one of these, add the counter that would have shown it
  (`render.scene_fallback_frames` / `render.frames_retained`,
  `watch.file_index_watcher_starts`), because the next one will not be found by
  reading code either;
- and write the regression test in the shape the real path uses. The first
  version of the exclude-glob test set the setting through `SetSettingValue` on a
  live shell and passed with the fix reverted — that path ends in
  `ApplyLiveSettings`, which seeds the very memo the test was meant to catch. The
  working version persists the setting and restores it in a fresh shell, which is
  what a launch does.

## Things That Break Silently Elsewhere

- **Bench and fuzz targets list explicit sources** rather than linking
  `microide_core`. Adding a dependency to a shared `.cpp` breaks their link — and
  fuzz targets break **silently**, because no default flow builds them. Smoke them
  with `tools/run-checks.sh fuzz --list`. Fuzz corpora contents are gitignored, so
  curated seeds need `git add -f`.
- **`microide_tests` has a curated list too**, and its link errors can hide from
  the default build. It picks up a handful of `tests/perf/*.cpp` without
  `PerfHarness.cpp`, so a new file there that CALLS the harness is an undefined
  reference — which the default build drops on the floor, because `--gc-sections`
  removes the unreferenced function and its relocations with it. The ASAN lane
  does not, so the break surfaced there, twenty minutes into a sanitizer sweep.
  Keep the half a test needs in a translation unit that depends on the metric
  STRUCTS and nothing else (`tests/perf/ScenarioAggregateWire.cpp` is the shape).
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
