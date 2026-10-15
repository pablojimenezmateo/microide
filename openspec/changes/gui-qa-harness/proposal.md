## Why

Three bugs reached the user in one afternoon that no test could see, because each
only shows on a real screen driven the way a person drives it: the editor painted
black while the Edit menu was open (a repaint skipped the editor against a menu
bar + popup bounding box), anti-aliased text drawn over itself on partial frames
(a nested clip replaced the dirty clip), and Ctrl+Shift+F doing nothing with the
tree focused (workbench shortcuts registered for the editor only). The unit suite
paints frames without the app's dirty hint and sends keys from one focus at a
time; the headless capture tooling takes stills but checks nothing. We need a
harness that drives the real window with real input, everywhere, with oracles that
need no expected screenshot -- including against a real remote host, where most
current development happens.

## What Changes

- **Redraw verifier** (`MICROIDE_QA_VERIFY_REDRAW=1`): after every partial frame
  the app renders the same prepared frame in full into a second offscreen target,
  compares the two, and logs `QA-STALE` with the differing region, the dirty rects
  that produced the frame and the event that requested them. Off by default; no
  cost when off. Turns every stale-pixel bug into a log line during any session,
  scripted or human.
- **`ui` control query**: the on-screen elements by name with their rects -- menu
  bar items, open popup rows (label, enabled), editor tabs, sidebar view buttons
  and visible rows, panel tabs, status-bar segments, banner buttons, notification
  buttons -- plus focus target, sidebar mode, overlay mode, menu state and the
  layout's surface rects. These are the locators a driver clicks by name.
- **Keybinding reachability check**: every built-in keybinding is pressed from
  every focus target it claims (and from the ones it must not reach, like a
  focused terminal) and must dispatch exactly there; runs in-process in the test
  suite and against the live window.
- **`tools/qa` driver** (Python, on the `tools/capture-media` recipe): isolated
  Xvfb + XDG per run, real input through xdotool (move, click, double-click, drag,
  wheel, chords, typing), control commands and queries, screenshots, and oracles
  (redraw verifier, sanitizer reports, crash, scenario expectations).
  - Scripted scenarios: open every menu and hover every row, press every visible
    button, drag every divider, split and close panes, open and dismiss every
    overlay, switch sidebar views, the terminal panel.
  - A seeded monkey: weighted random actions over named elements with every oracle
    armed; on a finding it saves the action list, which replays the bug.
  - Runs against any build, including the ASAN/UBSAN builds.
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
  render of the same state, how it reports, and its zero cost when off.
- `control-ui-query`: the `ui` query's contract -- element names, rects, state
  fields -- as the stable locator surface for automation.
- `gui-qa-harness`: the driver, scenario suite, monkey with replay, keybinding
  reachability matrix, remote scenarios, and how findings are triaged.

### Modified Capabilities
- `bug-detection-tooling`: adds the GUI QA lane (`run-checks.sh qa`) beside the
  sanitizer and fuzz requirements, and the rule that a QA finding lands with a
  unit regression test.

## Impact

- `src/app/Application.cpp` (+ a small verifier unit beside `SceneTexturePresenter`):
  a second offscreen render target and a readback when the verifier is on.
- `src/workspace/control/` (`ControlProtocol`, `ControlChannelService`): the `ui`
  query; element geometry comes from existing layout/hit-test helpers
  (`ComputePopupMenuRect`, tab strip, sidebar, panel layouts), not new state.
- `src/workspace/registries/WorkspaceKeybindingRegistry` and tests: the
  reachability matrix in `microide_tests`.
- New `tools/qa/` (Python 3 + Pillow, xdotool, xwd -- all already used by the
  media pipeline); `tools/run-checks.sh` gains `qa`.
- Docs: `dev-docs/control/control-channel.md` (the `ui` query),
  `dev-docs/project/validation-traps.md` (the verifier), a `tools/qa` README.
- No production behavior change; no new runtime dependency.
