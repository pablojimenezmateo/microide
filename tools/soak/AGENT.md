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

1. **Pick.** `tools/soak/soak.py report` (or `status`) lists open findings in
   fix order, each tagged with its impact tier (`soak.py` `priority_key`):
   `severe` (crash, sanitizer report, `data`, `undo`, `leak`), `behaviour`
   (`state`, `focus`, `expect`, timing), `layout` (`overlap`, `bounds`), then
   `paint` (stale frames). Within a tier a finding any real-size window shows
   comes before one tagged `/tiny` -- seen only where the LOGICAL window
   (pixels / ui.scale) is under 800x500, which the runner now rarely runs and
   keeps real-size runs out of (`--min-logical`). Work top down; a `paint/tiny`
   finding is worth fixing only as part of a class a bigger window also shows.
   Prefer a deterministic finding with a minimal replay; a flaky one needs its
   rate read before it is chased.
2. **Claim.** `tools/soak/soak.py ledger claim <sig> <your-name>`, so two agents
   never chase one defect.
3. **Reproduce.** Build the tree at HEAD (`cmake --build build --target microide`)
   and run the minimal replay from `FINDING.md`. It must reproduce on HEAD; if it
   does not, check whether a later commit fixed it (`git log` since the finding's
   commit) and mark it fixed with that commit, or add a note
   (`ledger note <sig> "..."`) and leave it for the runner to re-check.
4. **Understand.** Read the evidence: for stale paint, the `frame-*-retained.png` /
   `frame-*-reference.png` pair and the frame trace (which surfaces the partial
   frame repainted); for a crash or sanitizer report, the stack; for a `state`
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
   `tools/soak/soak.py ledger fixed <sig> <commit>`. The runner replays it once
   the commit is in its build and marks it `verified`, or `reopened` if it is
   still there. A later sighting reopens it as well. Verifying deletes the
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
