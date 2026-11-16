# The fixing agent's brief

The soak runner (`tools/soak/soak.py`) finds defects; you fix them. Each finding
arrives as a directory, `build/soak/findings/<signature>/`, whose `FINDING.md` has
what you need: the oracle that fired and its message, deterministic or flaky with
the rate, the commit and matrix cell, the minimal and full replay commands, the
stack for a crash, the thread dump for a stall, the log tail, screenshots and
stale-frame pairs, suspected files, and similar past findings with their fixes.

`CLAUDE.md` and `AGENTS.md` apply as for any change. In particular: proper fixes
only, a failing-first test for every fix, one commit per fix, and a TD entry in
`dev-docs/project/known-tech-debt.md` for anything deferred.

## The loop

0. **Take the machine.** The soak and the fixing agent take turns, each with
   every core: `tools/soak/soak.py pause "fixing" --wait 3600` stops new jobs and
   builds and returns once the running ones have ended. Replays, `rr` recordings
   and lanes then have the machine to themselves -- under a full soak the
   remote lanes time out and a replay's timing is not the user's. When the
   phase's fixes are pushed (or handed over), `soak.py resume` gives the
   machine back; it never lifts a containment pause.
1. **Pick.** `tools/soak/soak.py health` first: a cell that takes no steps or a
   recorder that is off is worth more than any one finding (the TSan cell was
   dead for four days). It also says what the soak does NOT reach -- the
   surface's gap list and the source coverage (§ What the soak does not reach)
   -- and splits the findings no replay reproduces by why. Then
   `tools/soak/soak.py report` (or `status`) lists
   open findings in fix order. Read the **families** list before the
   signatures: a data or undo signature keys on the FILE the oracle happened to
   check, so one defect is ten signatures (the kill -9 restore failure), and a
   family is what there is to fix. A finding marked **quiet** has had enough
   soak since its last sighting that its old rate was due three or more and
   none came -- probably gone; leave it unless you know better. At 4.6 due
   the runner books it `closed` (`close_quiet`) and any later sighting reopens
   it; close one by hand with `soak.py ledger close SIG REASON` when it was the
   harness's fault, not the app's. `[rr]` marks a
   finding with a recording (below). Each is tagged with its impact tier
   (`soak.py` `priority_key`):
   `severe` (crash, sanitizer report, `data`, `undo`, `leak`), `behaviour`
   (`state`, `focus`, `expect`, timing), `layout` (`overlap`, `bounds`), then
   `paint` (stale frames). Within a tier a finding any real-size window shows
   comes before one tagged `/tiny` -- seen only where the LOGICAL window
   (pixels / ui.scale) is under 800x500, which the runner now rarely runs and
   keeps real-size runs out of (`--min-logical`). Work top down; a `paint/tiny`
   finding is worth fixing only as part of a class a bigger window also shows.
   Prefer a deterministic finding with a minimal replay; a flaky one needs its
   rate read before it is chased ("0 of 12 replays": § A finding no replay
   reproduces). A rate measured "at the recorded pace" means
   the finding is a race: its replay record carries `"paced": true`, and
   `run.py replay` keeps the run's gaps between actions (`--paced` forces it on
   any record). A replay re-runs the data and undo oracles where the run ran
   them (`{"oracle": ...}` rows; records from before 2026-11-11 lack them, so an
   old undo finding never replays), and an undo finding keeps the text before
   and after as `undo-before.txt` / `undo-after.txt` in its evidence.
2. **Claim.** `tools/soak/soak.py ledger claim <sig> <your-name>`, so two agents
   never chase one defect.
3. **Reproduce.** Build the tree at HEAD (`cmake --build build --target microide`)
   and run the minimal replay from `FINDING.md`. It must reproduce on HEAD; if it
   does not, check whether a later commit fixed it (`git log` since the finding's
   commit) and mark it fixed with that commit, or add a note
   (`ledger note <sig> "..."`) and leave it for the runner to re-check.
4. **Understand.** Read the evidence: for stale paint, the `frame-*-retained.png` /
   `frame-*-reference.png` pair and the frame trace (which surfaces the partial
   frame repainted); for a crash or sanitizer report, the stack -- every build
   has one now: `crash-stack.txt` is the crashing thread's backtrace as the app
   printed it on its way down, resolved, and a crash's signature is keyed on it
   (two `microide exited (-6)` are two findings when their stacks differ); for an
   `invariant` finding, `invariants.json` (the rule the app itself says is
   broken) beside `tabs.json` / `documents.json` / `editor.json`, and the action
   that broke it is among the last five; for a `latency` finding, `latency.txt`
   (the main thread's time on a CPU, waiting for one, blocked, and its stack
   from the app's stall watchdog); `state.json` is the app's answers to its
   state queries when ANY finding was made; for a `state`
   finding, which label disagrees with which buffer fact; for `data`, which file
   and which bytes (`mismatch-<file>.buffer/.disk/.txt`) and the file's history --
   every read, write, adopted baseline and undo-to-clean the app logged for it
   (`mismatch-<file>.io.txt`, from `MICROIDE_TRACE_BUFFER_IO`). A `data`
   signature keys on the file's NAME only, so one signature holds every way that
   file has gone wrong: read the history before assuming the old mechanism; for `undo`, the action list before the sampled undo.
5. **Test first.** Write the failing test at the lowest level that shows the
   defect:
   - paint: `tests/support/RetainedRedraw.h` (`DispatchEvents`,
     `PaintAndDiffWithFullRepaint`) in `tests/QaFlowRegressionTests.cpp`;
   - a flow: a `QaFlowRegression/...` test, or a scenario in `tools/qa` when
     only the real window shows it;
   - editor state, data, undo: the subsystem's own tests.
   Run it and see it fail.
6. **Fix the cause**, not the symptom. If the bug is a class (the same mistake at
   several sites), fix every site and consider an architecture lint.
7. **Validate.** The focused tests, `tools/qa/run.py replay <minimal.json>` (now
   clean), the inner loop (`CLAUDE.md` § Development Workflow), and the lane the
   area needs (`qa` for paint/hover/focus, `asan` for memory).
8. **Commit** the test and the fix (one commit, or test then fix), then mark it:
   `tools/soak/soak.py ledger fixed <sig> <commit>` -- every signature of the
   family the fix covers. What happens next depends on whether a replay ever
   reproduced it. If one did, the runner replays it once the commit is in its
   build (as many times as its rate needs) and marks it `verified`, or
   `reopened` if it is still there. If none did, a replay proves nothing, and
   the finding stays `fixed` until the soak has run long enough on builds with
   the fix that its old rate was due 4.6 sightings and none came (`verified`,
   `by: exposure`; `status` shows the progress). A sighting from a build with
   the fix reopens it either way, at once. Verifying deletes the
   bundle's `evidence/`, `sightings/` and `verify/`; `FINDING.md`, `meta.json`
   and the replays stay. So copy any frame you want to keep before then.

   A reopened finding's `FINDING.md` still opens with the FIRST sighting. What
   the fix missed is under `sightings/` (listed in § Later sightings): the
   runner keeps the newest three sightings that came after a fix, or that carry
   the data oracle's byte evidence the bundle lacked. Read those first. A
   stale-paint (`log: QA-STALE ...`) signature keys on the reason, the clip
   count, the surface under the box, where its first pixel sits against the
   clips (`in-clip`: repainted wrongly; `clip-edge`: one pixel past a clip;
   `outside`: never damaged), whether it is a one-pixel line, and (since
   2026-11-11) the smallest `ui` element under the box (`button:search/replace-all`,
   `scrollbar:editor/vertical`, `-` for none), which the label shows in brackets.
   Older signatures lack some of these and can hold unrelated defects (250931ec505a
   held a lit button and a stray scrollbar); they stop being seen once the
   runner builds the change.
9. **Defer honestly.** Anything you cannot finish -- a second defect the fix
   exposed, a lint the class deserves -- goes into
   `dev-docs/project/known-tech-debt.md` as a TD entry, and a `ledger note`.

## A finding no replay reproduces

Most severe findings are these: a race, or a kill -9 that landed at one instant
in a million. Replaying the ACTIONS loses the timing, and on 2026-11-16 every
open severe finding measured 0%. Do not infer the mechanism from the code and
call it fixed -- that is how 35 "verified" findings came back.

First read WHICH zero it is (FINDING.md § Reproduction; `health` counts them).
The run stamps its state after every step and a replay compares its own:

- **the replays LEFT THE RUN'S PATH** -- FINDING.md names the first action after
  which the replay was somewhere the run was not. The record does not rebuild
  the run: a gap in the harness (`tools/qa`), and no recording will help until
  it is closed. Fix that first (a file the record names by a path that is gone,
  an outside-world step it did not record, a fixture that differs); it makes
  every finding of that shape replayable at once. Two such gaps were a sixth of
  all bundles.
- **NOT TESTED** -- the driver finished none of the replays (the error is
  quoted). The rate says nothing; fix the driver or `ledger note` why.
- **the replays followed the run's states to the end** -- the actions are not
  enough: timing. Then, in order:

0. **A kill at a named boundary.** A data finding whose label ends
   `(killed at durable.renaming@session#1)` came from the app killing ITSELF at
   that durable-write boundary (the `crash-points` query); its replay re-arms
   the same rule and dies at the same place. Those replay. The ones without
   the suffix are kills a signal landed -- the rest of this list.

1. **A recording.** About a third of the default-build jobs run under
   [rr](https://rr-project.org) (`+rr` / `+rr-chaos` in the cell), and a severe
   or behaviour finding keeps the run's traces (`evidence/rr/` or
   `sightings/*/rr/`; FINDING.md § Recording has the commands). A trace replays
   that exact execution -- same interleaving, same kill -- every time, and
   backwards: stop where the symptom is logged (`rr replay -M -a` tags each log
   line with its event; `rr replay -g <event>`), then `watch -l` the wrong
   value and `reverse-continue` to whoever wrote it. After a kill -9,
   `launch-N` is the killed process to its last instruction and `launch-N+1`
   the restore. The default build carries debug info for this. No recording
   yet: the finding recurs (its rate is in `status`), and the first recorded
   sighting is kept -- or make one now, step 2.
2. **A hunt.** `tools/soak/soak.py hunt SIG` replays the finding at the run's
   recorded pace under rr's chaos mode, up to `--runs` times, and keeps the
   first replay that shows it as a sighting of the bundle, trace and all. The
   runner does this itself for the severe findings without a recording, one at
   a time (`soak.py hunt` with no signature lists what it would hunt; a
   `hunt:` note in the ledger says how each ended), so look for a `-hunt`
   sighting before starting one. The monkey command FINDING.md also prints is
   NOT the same walk -- the run steered by the soak's coverage -- it looks for
   the finding again in its cell (`--rr` records it), it does not reproduce it.
3. **The whole log.** `evidence/run-logs/` is every launch's complete log, not
   the last 64 KB: with `MICROIDE_TRACE_BUFFER_IO` it is the file's full
   history. `evidence/machine.txt` says whether the machine was starved when a
   timing finding fired.
4. **More evidence next time.** If none of that explains it, add the trace line
   or the oracle evidence that would have, and let the soak bring the next
   sighting.

Recorded runs differ where rr does: one thread runs at a time, the app falls
back to SDL's software renderer (rr cannot record a GPU driver), the window is
at most 1440x900, and the latency and idle oracles are off. rr also stops at a
system call it does not implement; such a run is counted in `soak.py health`
and is not a finding (the installed rr is patched for the one met so far,
`tee`: TD-2026-11-16-451).

## What the soak does not reach

A defect in code no job executes is not found, however long the soak runs.
Two measures, both in `soak.py health`:

- **The surface** (`build/soak/surface-gaps.md`, written with every build): the
  app's own inventory (the `surface` query: commands, key bindings, settings,
  menu rows, overlay modes...) against what the runs reached. An item is
  reached, excluded with a reason and what covers it instead
  (`tools/soak/surface-extra.toml` `[exclude.*]`), or NOT REACHED -- and that
  list is work: a new action, profile or fixture in `tools/qa/monkey.py`. A key
  binding counts when it was pressed where it is live.
- **The source** (`build/soak/code-coverage.md`): what the jobs executed of
  `src/`, from a clang coverage build that runs under every profile in turn. By
  area, the files nothing ever entered, the largest unexecuted parts; `soak.py
  coverage --functions src/<path>` lists a file's uncalled functions. A whole
  area in the dark is a missing profile; a dark function inside a covered file
  is usually a state the monkey cannot get into -- say why in the monkey, or
  build the way in.

When a fix lands for something the soak found late or by luck, ask what would
have found it sooner and add that: an invariant to `ControlInvariantsQuery.cpp`
(the monkey asks it every five steps), a crash point at the write it concerns,
evidence to the oracle that fired.

## Not a defect of the app

Some findings are the monkey's: an action the app rightly refuses, an oracle too
strict for a legitimate state, a driver timing assumption. Fix the monkey or the
oracle (`tools/qa/`), with a negative control that shows the oracle still fires
on the real defect it exists for (`validation-traps.md` § vacuous fixtures), and
mark the finding fixed with that commit.

## A containment breach

Every scratch project lives under `build/`, inside the checkout. A run's git is
fenced to its project (`GIT_CEILING_DIRECTORIES` and file-only transports for the
monkey's git and the app's, a bare stand-in `origin` for a cloned project, and
the app's own ceiling in `GitCommandUtil`), and every 30 s the runner checks the
checkout's ref tips for the monkey's identity (`qa@example.com`) and `qa-branch-*`
names. On a hit it kills every running job, writes `build/soak/PAUSE` with what it
found, and records a `containment` finding. It stays paused until a person has
undone the damage (main, the `qa-branch-*` refs, the stash, anything pushed to
`origin`) and deleted `PAUSE`. Treat that finding as the most urgent there is: find
the path that got past the fences before the runner is unpaused.
