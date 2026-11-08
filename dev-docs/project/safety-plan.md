# Safety Plan: no crash, no lost data

Status: **adopted 2026-11-08**; first implementation pass the same day (see the tracker and TD-2026-11-08-435). Its centre is W1: the monkey, run continuously on this PC, with findings handed to a fixing agent. Owner of current direction is
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
- **The nightly is this PC, running continuously** (W1's soak runner). CI stays as it is:
  event-driven (`openspec/specs/ci-workflow-reliability` forbids cron), with fixed seeds as a
  fast regression gate. Everything open-ended runs on the soak runner, as one of its job
  types: fresh monkey seeds, kill -9 restarts, fault sweeps, wake races, fractional scales and
  long fuzzing.
- **No idle cost, no perf regression.** Debug-only invariants compile out of release.
  `idle_soak_30s` and the perf gate keep passing.
- **Lanes run from a pinned worktree** (`build/wt-lanes`), never the live tree.

## Order

| Phase | Workstreams | Why this order |
|---|---|---|
| 1 | W1.1 runner, W1.5 agent-ready findings, W1.2 coverage steering and corpus | The monkey is the bug finder and an agent is the fixer: first make it run forever and hand over fixable findings, then make it reach further |
| 2 | W4 data-loss oracle, W5 fault injection, W1.3 oracles | Data loss has no oracle yet; every new oracle multiplies what the monkey's reach finds |
| 3 | W2 assert-armed build, W3 cache invariants, W1.4 matrix, W7 scale, W8 sequence fuzzers | More environments and internal checks for the runner to rotate through |
| 4 | W6 wake-race lane, W9 pattern lints, W10 check the checks | Lock in the classes already found, and prove every check still bites |

## Workstreams

### W1. The monkey, as extensive as possible, on a continuous soak runner

**Why.** About 30 fresh seeds found 6 bugs today, and CI's fixed seeds had gone dry. Findings
will go to an agent that reviews and fixes them, so the monkey's job is to reach as much
behaviour as possible, check it with as many oracles as possible, and hand over every finding
ready to fix.

**Where it starts (2026-11-08).** `tools/qa/monkey.py`:
- picks uniformly from 7 action sources (element 50, chord 15, command 10, type 10, drag 5,
  wheel 5, point 5);
- types from a 7-string alphabet;
- drives one fixture project (`taskflow`) at one window size (1440x900, scale 1);
- checks crash, stale paint (redraw verifier), sanitizer, expectations and the state oracle;
- stops at its first finding.

#### W1.1 The soak runner (`tools/soak/soak.py`, a `systemd --user` service)

- **Follow `main`.** In its own pinned worktree (`build/wt-soak`, never the live tree), rebuild
  the release, debug-with-asserts, ASan, UBSan and TSan binaries when `main` moves. A broken
  build is a finding against that commit, and the runner keeps soaking the last good build.
- **Rotate jobs.** A weighted rotation over the matrix in W1.4. Each job has its own seed,
  output directory and hard timeout. The weights follow measured yield (W1.6).
- **A good neighbour.**
  - Everything runs under `nice -n 19` and `ionice -c3`, with a job cap (default: half the
    cores) and a pause file (`build/soak/PAUSE`).
  - It backs off on your own load and while a `run-checks` lane holds its lock.
  - Output stays in `build/soak/` (not the `/tmp` tmpfs) within a disk budget.
- **Status.** `soak.py status` prints uptime, jobs per hour, open findings by label, and the
  current commit and job.

#### W1.2 Reach more behaviour

- **Steer by coverage, not uniform choice.** A state signature per step, read from the `ui` and
  `buffer` queries: focus, overlay mode, sidebar mode, active tab kind, menu open, panel
  content, split count, modal up, soft wrap, selection kind. The monkey keeps a novelty score
  and biases toward actions that reached unseen states and transitions. Command coverage
  (which of the ~270 commands have run, and in which states) steers command choice.
- **A corpus of interesting prefixes**, as a fuzzer keeps one. A run that reaches a new state
  saves its action prefix (`build/soak/corpus/`); later runs replay a saved prefix and explore
  from there, so deep states (a merge mid-conflict, a debug session paused, a split of a soft
  wrapped file with the find widget open) are reached again and widened instead of found once
  by luck.
- **Keep going after a finding.** A non-fatal finding (stale paint, an expectation) is
  recorded and exploration continues. A crash records its evidence, then the app relaunches
  with the same profile directory, which also exercises session restore.
- **A wider action vocabulary.**
  - *Text:* Unicode (CJK, emoji, combining marks, RTL), tabs, CRLF, very long lines, huge
    pastes, and IME composition events.
  - *Mouse and keyboard:* every modifier on clicks and drags, triple and right clicks, text
    drag and drop, file drops, key-repeat bursts, and bursts with no settle between actions
    (timing stress).
  - *Window:* resize from tiny to huge, maximize and fullscreen, focus loss and regain.
  - *Settings:* `ui.scale`, theme, fonts, tab size, wrap, sticky scroll, code lens placement,
    each reachable as an action, not only by clicking through Settings.
  - *Workspace:* project switching and multiple projects; terminal work (run, interrupt,
    resize, a full-screen TUI); git operations from the UI and from outside (commit, checkout,
    rebase with conflicts, stash).
- **External worlds.** Extend the churn agent to create, delete and rename files and whole
  directories, change permissions, add symlinks and loops, and fill the disk to near-full
  (W5). Fake LSP and DAP servers answer at random latencies, out of order, late and duplicated
  (W8). A loopback-sshd remote project runs with a slow link (`qa-remote`'s setup).

#### W1.3 Check more on every step

The existing oracles (crash, stale paint, sanitizer reports, state oracle, expectations), plus:

- **Data integrity** (W4): a shadow model of every save and dirty buffer, checked byte for byte
  at the end and after every kill -9 and relaunch.
- **Internal invariants** (W2, W3): an assert-armed build in the rotation, and a debug control
  command that verifies every view's caches and the frame-prep invariants, called every N
  steps.
- **Undo algebra**, sampled: snapshot the buffer, undo k steps, redo k steps, compare (the
  invariants tool's rules, mid-run instead of on a fixture).
- **Liveness:** every control command answers within a budget (the TD-433 class). After a quiet
  period, the idle wake count is zero.
- **Latency:** the time from input to the frame that shows it, over a budget, is a finding,
  with the redraw trace attached.
- **Leaks:** RSS slope, open file descriptors, threads and child processes from `/proc`,
  sampled. Growth over a run beyond a budget is a finding.
- **The app's own log:** any assertion, `error`, sanitizer line, or "should not happen" notice
  is a finding.

#### W1.4 The matrix the runner rotates through

- **Builds:** release, debug-with-asserts, ASan, UBSan, TSan. On a sanitizer build the slow
  factor applies.
- **Display:** X11 and Wayland; window sizes from 640x400 to 2560x1440; `ui.scale` 1, 1.25,
  1.5, 1.75 and 2 (W7); light and dark themes.
- **Fixtures:**
  - this repository, `taskflow`, and a generated set: a 50 MB file and a 1 MB single-line file;
  - binary and invalid UTF-8; CRLF, mixed endings and Latin-1;
  - deep trees and Unicode file names;
  - a repo with conflicts, detached HEAD, submodules and a large history;
  - an empty folder;
  - a remote project.
- **Profiles:** default, editor and commands, plus new focused profiles (merge and compare,
  terminal, git workflows, settings, multi-project, debugger), each with its own weights.

#### W1.5 Findings an agent can fix without re-deriving them

- **Minimise every deterministic finding.** Delta debugging (ddmin) over its replay finds the
  shortest action sequence that still reproduces it, on the same build. The minimal replay
  goes into the bundle beside the full one.
- **One directory per finding** (`build/soak/findings/<id>/`), with a `FINDING.md` holding:
  - the symptom and the oracle that fired, and the signature;
  - deterministic or flaky, with the rate;
  - the commit and the environment cell from W1.4;
  - the minimal and full replay commands;
  - for a crash, the symbolized stack (core dump or gdb), and for a stall, the thread dump;
  - the app log tail and before/after screenshots or the stale-frame pair;
  - suspected files (from the stack, or the redraw trace's surfaces);
  - similar past findings from the ledger, with their fix commits and tests.
- **A ledger with a lifecycle** (`build/soak/ledger.jsonl`):
  - states: new, claimed, fixed with its commit, then verified, or reopened;
  - deduplication by signature, so a recurring defect raises its count instead of opening a
    new entry;
  - the runner re-checks a fixed signature on every later commit and reopens it if it
    returns.
- **`tools/soak/AGENT.md`**, the brief the fixing agent follows:
  - claim a finding;
  - reproduce it from the minimal replay;
  - write the failing-first unit test (`tests/support/RetainedRedraw.h` for paint;
    QaFlowRegression for flows) and the fix, one commit each, following `CLAUDE.md`;
  - mark it fixed with the commit;
  - file a TD for anything deferred.
- **Notifications.** `soak.py report` writes `build/soak/report.md` (new deterministic first,
  then flaky, then yield), and `notify-send` fires on each new deterministic finding.

#### W1.6 Measure the monkey itself

- **Coverage:** states and transitions seen, commands run per state, and source coverage from
  a periodic gcov build in the rotation (which files and branches no job reaches).
- **Yield:** findings per CPU-hour per job type and per matrix cell. The rotation weights
  follow it, and a plateau (no new state or finding in N hours) shifts effort to the least
  covered areas.
- **Gaps:** the weekly report lists the least-reached code and commands, as input for new
  profiles and actions.

#### W1.7 The whole surface, inventoried and covered

"As much surface as possible" has to be measurable, so the target is an inventory, not a
feeling.

- **An inventory generated from the app** wherever possible, so it cannot drift:
  - every registered command;
  - every keybinding, every menu and context-menu item, and every Settings key;
  - every element kind and name the `ui` query reports;
  - every overlay mode, sidebar view, panel content and tab kind;
  - every control-channel command;
  - the language and file-type registry.

  Exported by a `surface` control query; `tools/soak/surface.py` diffs it per commit, so a new
  feature appears as uncovered the day it lands. A hand-kept list (`tools/soak/surface-extra.toml`)
  covers what cannot be enumerated, each item with an owner: external events, plugin APIs and
  remote operations.
- **Coverage at three depths.**
  - *Reached:* every item exercised at least once, by the monkey or a checking QA flow. The
    target is 100%, and an item nothing reaches is a finding against the monkey.
  - *Reached in context:* each action in each state it applies to, pairwise over the W1.2 state
    signature × action. For example, `cut` with and without a selection, in an editor, a
    compare pane, a merge result and a read-only buffer.
  - *Code:* gcov line and branch coverage from a periodic instrumented job. Every unreached
    branch is either targeted next or explained.
- **A weekly gap report:** items never reached, items reached only in their default state, and
  the least-covered files. Each gap becomes a new action, profile, fixture or external-world
  event.
- **Nothing uncovered by accident.** Surface the monkey cannot drive from inside (native OS
  dialogs, IME candidate windows, suspend and resume, display hotplug, drags from other apps,
  clipboard set by other apps) gets either a control-channel hook that injects the event the OS
  would deliver (as `pointer` and `key` do for Wayland) or an entry in an exclusion list with
  its reason and the test that covers it instead.

**Acceptance**
- The runner survives 24 h unattended, with no crash, no disk growth past its budget, and no
  noticeable latency alongside interactive work.
- A planted stale-paint defect and a planted lost-save defect on `main` are each found,
  minimised to fewer than 20 actions, deduplicated, and handed over with a `FINDING.md`
  within a few hours.
- After the revert lands they are marked verified, and they reopen if planted again.
- State and command coverage rise over a week and are reported.
- Every inventoried item is reached, or is on the exclusion list with its reason and covering
  test.
- Pairwise state × action coverage and gcov branch coverage are reported per commit and do not
  regress.

**Size:** L, the largest workstream and the first. It is built incrementally: the runner and
findings ledger first, then minimisation and `FINDING.md`, then coverage steering, then
vocabulary, matrix and oracles in order of measured yield.

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
- A `kill -9` step at random monkey steps (a soak job type), then relaunch:
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
- Both run as soak job types (W1); a short version also runs in the lane.
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
- Corpora committed under `tests/fuzz/corpora/`; each runs 60 s in the `fuzz` lane, and in
  long rounds on the soak runner (W1), whose new corpus entries are merged back on each session.

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
| W1.1 | Soak runner | 1 | built, not yet soaked 24 h | `tools/soak/soak.py` (e0ebfd31c), build spacing (708124f44), systemd unit |
| W1.5 | Agent-ready findings (minimise, FINDING.md, ledger, AGENT.md) | 1 | built | ledger, `minimize.py` (README stale buffer: 410 -> 50 actions), `AGENT.md` (e0ebfd31c) |
| W1.2 | Coverage steering, prefix corpus, wider vocabulary | 1 | mostly done | 15 sources, 8 profiles, steering, corpus (e0ebfd31c); menu follow-through (708124f44); driver 5x faster (e5add2866); gaps in TD-435 item 5 |
| W1.3 | Per-step oracles (liveness, latency, leaks, log, undo algebra) | 2 | partly done | data, undo, leaks, liveness, staging leftovers (e0ebfd31c); frame latency, idle wakes, log levels: TD-435 item 6 |
| W1.4 | Builds, display, fixtures and profiles matrix | 3 | partly done | default/release/ASan/UBSan/TSan x 8 profiles x sizes x ui.scale in the soak; Wayland and big fixtures: TD-435 item 5 |
| W1.6 | Monkey coverage and yield measurement | 3 | partly done | coverage file, yield-weighted rotation, `soak.py report`; gcov and plateau: TD-435 item 7 |
| W1.7 | Surface inventory, 100% reached, pairwise and gcov depth, exclusions | 1-3 | partly done | `surface` query (667c0d529), `surface.py` + `surface-extra.toml` (3436d07a7); pairwise/gcov: TD-435 item 7 |
| W4 | Data-loss oracle | 1 | done | `documents` query (52319aeb4), oracle (e0ebfd31c), `save-round-trip` (fa28dfc6a); both planted defects caught (a dropped byte on save; a dirty buffer restored empty after kill -9); found the watcher symlink bug (99d4a63a0) |
| W5 | Save and persistence fault injection | 1 | partly done | `LocalWriteFaultMatrix` + planted non-atomic control (e73173c0a); errno faults, more crash points, inventory: TD-435 item 3 |
| W2 | Assert-armed QA plus prep invariants | 2 | partly done | the default build is assert-armed (no NDEBUG): `qa` already is the lane; prep invariants: TD-435 item 2 |
| W3 | Per-view cache invariants | 2 | done | `VerifyDerivedCachesForTesting` + multi-view sweep, 10 000 rounds clean, bites with the TD-426 fix loosened (fbb80ddff) |
| W9 | Pattern lints | 2 | partly done | `CheckEditorMetricsNameTheirStickyBand` (47689624b); range-for lint widened (3436d07a7); rules 2-3: TD-435 item 1 |
| W6 | Wake-race lane | 3 | done | `run-checks.sh wake-race` (0aac53ca1): 0 stalls with the relay, 8 (X11) and 3 (Wayland) without, per 240k rounds |
| W7 | Fractional-scale QA | 3 | done (lane) | `run-checks.sh qa-scale`, `MQA_UI_SCALE` (0aac53ca1); window sizes in layout units and the run's size applied at startup (ef59309d3); the soak rotates ui.scale |
| W8 | Sequence fuzzers | 3 | not started | TD-435 item 4 (the multi-view unit sweep covers the viewport half) |
| W10 | Check the checks | 4 | partly done | `tools/check-invariants-bite.sh` + 10 recorded controls (e73173c0a); mutation testing, guard audit: TD-435 item 8 |

Update a row's status and commits as each lands. A workstream is **done** when its acceptance
criteria hold, its lane or test is green in CI (`checks` + `qa-deep`) on a push to main, and
its long-running form is a job on the soak runner.
