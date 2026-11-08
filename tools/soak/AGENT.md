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

1. **Pick.** `tools/soak/soak.py report` (or `status`) lists open findings,
   deterministic ones first. Prefer a deterministic finding with a minimal
   replay; a flaky one needs its rate read before it is chased.
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
   and which bytes; for `undo`, the action list before the sampled undo.
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
   still there. A later sighting reopens it as well.
9. **Defer honestly.** Anything you cannot finish -- a second defect the fix
   exposed, a lint the class deserves -- goes into
   `dev-docs/project/known-tech-debt.md` as a TD entry, and a `ledger note`.

## Not a defect of the app

Some findings are the monkey's: an action the app rightly refuses, an oracle too
strict for a legitimate state, a driver timing assumption. Fix the monkey or the
oracle (`tools/qa/`), with a negative control that shows the oracle still fires
on the real defect it exists for (`validation-traps.md` § vacuous fixtures), and
mark the finding fixed with that commit.
