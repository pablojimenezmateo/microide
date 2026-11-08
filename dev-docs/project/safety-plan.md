# Safety Plan: no crash, no lost data

Status: **adopted 2026-11-08**, all ten workstreams to be done. Owner of current direction is
still `active-work.md` (§ 8 links here); this file is the program's detail and its tracker.

## Why now

The 2026-11-08 session (TD-2026-11-08-434, in
`guidelines/tech-debt/archive/2026-11-08-resolved-debt-ledger.md`) found nine real defects. Six
were found by the QA monkey on fresh seeds and none by the existing lanes on their fixed seeds:

- a silent layout corruption that had been open since October (two views of one document);
- a lost wake inside SDL that froze the control channel (TD-433);
- five repaint and hit-test bugs, all from one recurring mistake (a grid laid out without the
  sticky band);
- a test guard that was wrong for its whole life (UBSan builds were not sanitizer builds).

The lesson is that the defects live where no oracle looks. Data loss has no direct oracle at all
today: the lanes check pixels, crashes and sanitizer reports, but never the bytes on disk.

## Rules for every workstream

- **Evidence first.** Each new check lands with a negative control that shows it can fail: a
  reverted fix, an injected fault, or a fixture that must trip it
  (`validation-traps.md` § vacuous fixtures).
- **Every finding becomes a failing-first test plus its fix**, one commit each, as today.
- **CI stays event-driven.** `openspec/specs/ci-workflow-reliability` forbids cron, so a push to
  `main` is the nightly (as `qa-deep` already is). Longer soaks run locally through
  `tools/run-checks.sh`.
- **No idle cost, no perf regression.** Debug-only invariants compile out of release.
  `idle_soak_30s` and the perf gate keep passing.
- **Lanes run from a pinned worktree** (`build/wt-lanes`), never the live tree.

## Order

| Phase | Workstreams | Why this order |
|---|---|---|
| 1 | W1 fresh-seed monkey, W4 data-loss oracle, W5 save fault injection | Highest yield per hour, and data loss has no oracle yet |
| 2 | W2 assert-armed QA, W3 per-view cache invariants, W9 pattern lints | Turn silent corruption into crashes the Phase 1 tools then find |
| 3 | W6 wake-race lane, W7 fractional-scale QA, W8 new fuzzers | Close the environment- and input-shaped blind spots |
| 4 | W10 check the checks | Audit everything above, and what came before it |

## Workstreams

### W1. Fresh-seed monkey on every push to main

**Evidence.** About 30 fresh seeds found 6 bugs today. CI's fixed seeds (`seed 1`, asan
`1-4`) had gone dry.

**Deliverables**
- `qa-deep` derives N seeds from the commit SHA, so each push explores new ground. The profiles
  split as default 500 steps, editor and commands, on both X11 and Wayland (the `seeds` input
  already exists for dispatch).
- `tools/qa/run.py triage <out>` replays every finding three times and labels it deterministic or
  flaky; the replay command goes into the job summary.
- `tools/run-checks.sh qa-soak [N]` as the local equivalent (default 40 seeds), sized for this
  16-core machine.
- A seed ledger (`build/qa-seeds.log`, plus the CI artifact) so a seed is never re-spent
  unknowingly.

**Acceptance**
- A push runs a disjoint seed set from the previous push.
- A planted stale-paint defect is found and labelled deterministic by triage.

**Size:** S–M.

### W2. QA on an assert-armed build, with more frame-prep invariants

**Evidence.** The width-table bug corrupted layout silently in release and only aborted in a
debug build. The new focus-ring assert (`WorkspaceShellRenderFrame.cpp`) is the pattern to
repeat.

**Deliverables**
- A `qa-debug` lane: the GUI suite plus a monkey wave on a debug build with assertions on (no
  sanitizer, so it stays fast).
- Debug-only invariants at frame-prep boundaries:
  - every surface view model is built after the last state mutation of the frame (generalising
    the focus assert);
  - the painted tooltip, hover and caret owner agree with the input owner;
  - no prep step requests damage outside the window.
- A short note in `guidelines/testing.md`: when a bug was silent, add the assert that would have
  made it loud.

**Acceptance**
- Each invariant has a negative control: the corresponding past bug, reverted, trips it in a
  unit test.

**Size:** M.

### W3. Per-view cache invariants

**Evidence.** Two views of one document each keep private width and wrap tables spliced only by
their own edits. That bug class went unseen for weeks.

**Deliverables**
- `TextViewport::VerifyDerivedCachesForTesting()` (debug and tests): every table that claims
  current equals a fresh measure, for every view of the document, not only the editing view.
- A hook: after each edit in debug builds (sampled to keep it cheap), verify all live views of
  the document, wired through the document's view registry.
- A stateful unit fuzz test: 2–4 views at different widths, soft wrap on and off, random edits
  through random views with undo and redo, verifying all views after each step.

**Acceptance**
- With fc0f5a1a7 reverted the fuzz test fails within its default iterations; with it applied it
  passes 10,000 iterations.

**Size:** M.

### W4. A data-loss oracle for the monkey

**Evidence.** Nothing checks the bytes on disk today.

**Deliverables**
- A shadow model in the monkey driver. It records, per file:
  - the last content saved, read back through the control channel's `buffer` and on save
    completion;
  - the dirty buffers' contents.
- End-of-run oracle: each saved file on disk equals its last saved content, byte for byte,
  including line endings, encoding and final newline.
- A `kill -9` step at random monkey steps, then relaunch:
  - every dirty buffer comes back with its content (hot exit and the crash-safety mirror);
  - every saved file is intact;
  - no temp file is left behind.
- External-change churn stays on, so conflicts are part of the model: the oracle accepts either
  side only where a prompt was answered.

**Acceptance**
- A planted defect that drops the last keystroke on save is caught.
- A planted defect that loses one dirty buffer on restore is caught.

**Size:** M–L.

### W5. Fault injection across save and persistence

**Evidence.** `util/CrashPoint.h` exists with 16 sites. Remote saves were exercised under faults
(TD-430); local saves and persistence were not swept systematically.

**Deliverables**
- A `faults` lane (in `run-checks.sh`): for every crash point × {crash, ENOSPC, EIO, EACCES,
  EXDEV on rename, a 5 s fsync stall}, in a subprocess harness, assert:
  - each target file is the old or the new content, never truncated or mixed;
  - the session and user config load (a corrupt record is skipped with a notice, not a crash);
  - the user sees an error for every failed save;
  - no temporary is left beyond the sweep's rules (9ca11c328).
- Crash points added where the sweep shows gaps: session write, user config write,
  replace-in-files multi-file apply, merge write, rename/move.
- An inventory table of every write path and its atomicity guarantee in
  `dev-docs/project/editor-essentials.md`.

**Acceptance**
- Every write path in the inventory has at least one injected fault in the sweep.
- A planted non-atomic write fails the lane.

**Size:** L.

### W6. Wake and main-loop race coverage

**Evidence.** TD-433 was SDL 3.4's lost cross-thread wake. Our code is TSan-clean, but nothing
contended the main loop's wait boundary.

**Deliverables**
- `tools/qa/stress/sdl_wake_race.cpp` built by CMake as a tool target, run by a `wake-race`
  lane on X11 (Xvfb) and Wayland (weston) with 12 copies, as a check of the relay. It must show
  zero stalls with the relay; without the relay it runs as an informational probe that reports
  SDL's own behaviour, so an SDL bump that fixes the race is visible.
- The same lane covers `SDL_RunOnMainThread` and termination-signal wakes (both use the same SDL
  path).
- An upstream SDL issue filed with the stress program and the analysis (draft from the TD-433
  entry).

**Acceptance**
- The lane fails when `LostWakeRelay::NotePush` is stubbed out.

**Size:** S.

### W7. Fractional-scale QA

**Evidence.** Seeds 602 and 605 both happened after a monkey zoom. Every lane runs at 1× or 2×.
The software canvas cannot mimic the GPU path exactly (resampling noise), so the real check has
to be a GUI one.

**Deliverables**
- `qa-scale`: the GUI suite (a scale-sensitive subset first: layout, split, scrollbars, hover,
  menus) and a monkey wave at `ui.scale` 1.25 and 1.75 on X11, redraw verifier on.
- The monkey's zoom actions weighted up in one profile so fractional states are reached more
  often.
- Keep `QaFlowRegression/PartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit` as the
  in-process guard and extend its sweep to the bottom panel's top edge and the right pane.

**Acceptance**
- A planted one-device-column clip error at 1.75× is caught by `qa-scale`.
- The lane is clean or every finding is filed.

**Size:** S–M.

### W8. Fuzzers where untrusted bytes and sequences enter

**Evidence.** The 17 libFuzzer targets are byte parsers. Today's stale-result bugs (rename,
format, code actions: commits 4eee951b0, da84aa8c7, 009961dd5) are sequence bugs, which no
fuzzer generates.

**Deliverables**
- `ControlCommandFuzz`: structured command lines against a headless shell, asserting no crash or
  hang and a reply per command.
- `LspSessionFuzz`: a scripted fake server answering requests out of order, late, duplicated or
  for older document versions, interleaved with edits. The oracle is no crash and no edit
  applied to text it was not computed for (the version and revision checks hold).
- `DapSessionFuzz`: the same shape for the debug adapter.
- `ViewportEditSequenceFuzz`: multi-view TextViewport edits, undo, redo and reload against a
  plain-string reference model (extends `PieceTreeEquivalenceFuzz` up a layer; shares W3's
  verifier).
- Corpora committed under `tests/fuzz/corpora/`; each runs 60 s in the `fuzz` lane.

**Acceptance**
- Each target finds a planted bug of its class (a dropped version check, an undo that skips a
  view) within its 60 s.

**Size:** L.

### W9. Lints for the patterns that recurred

**Evidence.** Five call sites had the zero-sticky-band bug. One hit-test bypassed the modals.
The floating widgets were missing from the hover catch-all.

**Deliverables** (architecture rules, each with positive and negative fixtures):
- `CheckEditorMetricsNameTheirStickyBand`: a `ComputeMetrics(..., 0, ...)` call in
  `src/workspace` needs an adjacent comment saying why the band is ignored (the two-step sizing
  callers); otherwise pass `sticky_band_rows()`.
- `CheckHitTestsHonourModals`: mouse hit-test entry points that move focus must go through
  `FindSingleLineInputHit` or check `prompts.surface_visible` / `OverlayIsModal`.
- `CheckPointerOverElementsAreHoverTargets`: every rect painted with `PointerOver(` in a render
  TU is reachable from `HoveredInteractiveRect` (by a shared helper or an allowlist with a
  reason).

**Acceptance**
- Each rule fails on today's pre-fix code (its negative fixture) and passes on HEAD.

**Size:** M.

### W10. Check the checks

**Evidence.** The UBSan timing guard was wrong from day one, and three negative controls today
showed a probe passing for the wrong reason until it was strengthened.

**Deliverables**
- An audit of every timing guard and sanitizer branch (`IsSanitizerBuild`, `Trustworthy`,
  `#if` sanitizer macros): each branch is reached by at least one lane, recorded in a table in
  `validation-traps.md`.
- `tools/check-invariants-bite.sh`: for each architecture rule, invariant assert and oracle from
  W2–W9, apply its recorded negative control (a patch under `tools/negative-controls/`) and
  require the check to fail.
- Mutation testing (mull or a sed-based mutator) over the oracle code itself: the QA driver's
  invariants, `RedrawVerifier`, and the data-loss oracle. Surviving mutants become TDs.
- A recurring line in each session's TD summary: which checks were re-proven to bite.

**Acceptance**
- The bite script passes on HEAD and fails when any single negative control is not detected.

**Size:** M.

## Tracker

| WS | Title | Phase | Status | Commits / evidence |
|---|---|---|---|---|
| W1 | Fresh-seed monkey on every push | 1 | not started | |
| W4 | Data-loss oracle | 1 | not started | |
| W5 | Save and persistence fault injection | 1 | not started | |
| W2 | Assert-armed QA plus prep invariants | 2 | not started | focus-ring assert landed (640f0894c) |
| W3 | Per-view cache invariants | 2 | not started | `split-wrap-edits` flow landed (fa542a981) |
| W9 | Pattern lints | 2 | not started | |
| W6 | Wake-race lane | 3 | partly done | relay and stress probe landed (38b167725) |
| W7 | Fractional-scale QA | 3 | partly done | in-process probe landed (f92a0a4ff) |
| W8 | Sequence fuzzers | 3 | not started | |
| W10 | Check the checks | 4 | not started | ubsan guard fixed (43fcca3ed) |

Update a row's status and commits as each lands. A workstream is **done** when its acceptance
criteria hold and its lane or test is green in CI (`checks` + `qa-deep`) on a push to main.
