## Why

Three bugs reached the user in one afternoon that no test could see, because each
only shows on a real screen driven the way a person drives it: the editor painted
black while the Edit menu was open (2383101b: a repaint skipped the editor against
a menu bar + popup bounding box), anti-aliased text drawn over itself on partial
frames (2ef0397a: a nested clip replaced the dirty clip), and Ctrl+Shift+F doing
nothing with the tree focused (db16f4a2: workbench shortcuts registered for the
editor only). The unit suite paints frames without the app's dirty hint and sends
keys from one focus at a time; the headless capture tooling takes stills but
checks nothing.

A fourth case, from the first runs of the draft driver while this proposal was
being verified, shows what the harness must record: the File menu opened over a
BLANK sidebar column (tree and search view alike, from a fresh instance) four
times in a row, then never again in twenty launches under every timing and load
we could think of. Nothing captured the frame sequence that produced it, so it is
filed (TD-2026-10-15-343) with a sequence and a screenshot instead of a cause.
Finding is not enough: every finding has to land with the frames that led to it.

We need a harness that drives the real window with real input, everywhere, with
oracles that need no expected screenshot, that keeps the evidence for anything it
sees -- including against a real remote host, where most current development
happens.

## What Changes

- **Redraw verifier** (`MICROIDE_QA_VERIFY_REDRAW=1`): after every partial frame
  the app renders the same prepared frame in full into a second offscreen target,
  compares the two, and logs `QA-STALE` with the differing region, the dirty rects
  that produced the frame and the event that requested them. Off by default; no
  cost when off. The pixel diff is one pure function shared with the unit suite's
  retained-redraw tests (which today carry their own copy in
  `tests/support/RetainedRedraw.h`), and the verifier is proven able to fire by an
  in-process test that forces a wrong skip and expects the report.
- **`ui` control query**: the on-screen elements by name with their rects -- menu
  bar items, open popup rows (label, enabled), editor tabs, sidebar view buttons
  and visible rows, panel tabs, status-bar segments, banner buttons, notification
  buttons -- plus focus target, sidebar mode, overlay mode, menu state and the
  layout's surface rects. These are the locators a driver clicks by name.
- **Keybinding reachability check**: every built-in keybinding (92 today) is
  pressed from every focus target it claims (and from the ones it must not reach,
  like a focused terminal) and must dispatch exactly there; runs in-process in the
  test suite and against the live window.
- **`tools/qa` driver** (Python, on the `tools/capture-media` recipe): isolated
  Xvfb + XDG per run, real input through xdotool (move, click, double-click, drag,
  wheel, chords, typing), control commands and queries, screenshots, and oracles
  (redraw verifier, sanitizer reports, crash, scenario expectations).
  - Evidence per finding: the app's own redraw trace is on for every run, and a
    finding is written with the trace lines and the screenshots of the actions
    before it, plus the replay file.
  - Scripted scenarios: open every menu and hover every row, press every visible
    button, drag every divider, split and close panes, open and dismiss every
    overlay, switch sidebar views, the terminal panel.
  - A seeded monkey: weighted random actions over named elements with every oracle
    armed; on a finding it saves the action list, which replays the bug. The
    control-command fuzzer that already exists (`tools/fuzz-control-commands.py`:
    seeded command streams with on-disk churn, built for the ASAN binary) becomes
    one of the monkey's action sources, and the overlay-backdrop repro script
    becomes a scenario; both files are deleted once absorbed, so there is one
    launcher, one isolation recipe and one set of oracles.
  - Runs against any build, including the ASAN/UBSAN builds, and safely beside
    another driver on the same machine (Xvfb picks the display).
- **Remote scenarios** against an ssh host (`worky` here; any host via
  `MQA_REMOTE_HOST`): open a host folder, host edits under open files, a save
  conflict, the link dropped and restored, a host terminal, simulated agents
  churning the tree while menus, search and splits are open.
- **`tools/run-checks.sh qa`** lane running the local scenario suite and a fixed
  monkey seed headlessly; the remote suite runs on demand.
- Every finding becomes a regression test in the unit suite (the harness finds,
  the suite pins) and its own fix commit; findings not fixed are filed as TD.

## Capabilities

### New Capabilities
- `redraw-verification`: the in-app check that a partial frame equals a full
  render of the same state, how it reports, its zero cost when off, and the proof
  that it can fire.
- `control-ui-query`: the `ui` query's contract -- element names, rects, state
  fields -- as the stable locator surface for automation.
- `gui-qa-harness`: the driver, scenario suite, monkey with replay, keybinding
  reachability matrix, remote scenarios, the evidence a finding carries, and how
  findings are triaged.

### Modified Capabilities
- `bug-detection-tooling`: adds the GUI QA lane (`run-checks.sh qa`) beside the
  sanitizer and fuzz requirements, moves the control-command fuzzer into the
  harness, and adds the rule that a QA finding lands with a unit regression test.

## Impact

- `src/app/Application.cpp` (+ `src/app/RedrawVerifier.{h,cpp}` beside
  `SceneTexturePresenter`): a second offscreen render target and a readback when
  the verifier is on. The pure diff lives in `src/render/PixelDiff.{h,cpp}` and
  `tests/support/RetainedRedraw.h` consumes it instead of its own loop.
- `src/workspace/control/` (`ControlProtocol`, `ControlChannelService`, new
  `ControlUiQuery`): the `ui` query; element geometry comes from the calls the
  mouse paths already make (`ComputePopupMenuRect`/`HitTestPopupRow`,
  `WorkspaceTabStripChrome::ComputeVisible*Tabs`, `SidebarModeRow`, the sidebar
  list layouts, `ComputeEditorBannerButtonRects`, `NotificationHitAt`,
  `ForEachStatusBarSegmentRect`), not new state.
- `src/workspace/coordinators/WorkspaceKeyInputCoordinator` and
  `WorkspaceKeybindingRegistry` tests: the reachability matrix in `microide_tests`.
- `src/workspace/CaretRedraw.cpp` and the five render sites that call
  `CaretVisibleNow()` at paint time: the blink is decided once per prepared frame
  (the editor caret already is), so a second render of the frame is identical.
- New `tools/qa/` (Python 3 + Pillow, xdotool, xwd -- all already used by the
  media pipeline); `tools/run-checks.sh` gains `qa`;
  `tools/fuzz-control-commands.py` and `tools/repro/overlay-backdrop-repro.sh`
  are absorbed and deleted.
- Docs: `dev-docs/control/control-channel.md` (the `ui` query),
  `dev-docs/project/validation-traps.md` (the verifier, and the finding without
  its frames), a `tools/qa` README, `dev-docs/project/media-generation.md` (the
  repro script's replacement).
- No production behavior change beyond the paint-time blink read; no new runtime
  dependency.
