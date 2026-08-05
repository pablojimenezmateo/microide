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
