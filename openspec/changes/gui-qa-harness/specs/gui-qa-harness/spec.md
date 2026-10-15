## ADDED Requirements

### Requirement: A driver runs the real application in isolation

`tools/qa` SHALL provide a Python driver that starts any microide binary on a private Xvfb display with its own XDG config, state, data and runtime directories, delivers real input through xdotool (pointer moves, clicks, double-clicks, drags, wheel, key chords, typed text), issues control commands and queries, takes screenshots, and stops everything it started. A run SHALL NOT read or write the user's real configuration, session or control sockets, except that a remote run MAY use the user's real HOME for ssh keys and configuration.

#### Scenario: A run leaves the user's state alone
- **WHEN** a QA run starts and stops
- **THEN** nothing under the user's XDG directories changed, and the display, processes and work directory it created are gone

#### Scenario: Elements are driven by name
- **WHEN** a scenario clicks `menu:Edit`
- **THEN** the driver resolves the name through the `ui` query and clicks inside that element's rect

### Requirement: Oracles need no expected screenshots

Every QA action SHALL be followed by a check of the always-on oracles: `QA-STALE` lines from the redraw verifier, sanitizer reports (ASan, LSan, UBSan, TSan), assertion failures, and the process having exited. Scenario-specific expectations SHALL be stated against the `ui` query or control queries, not against reference images.

#### Scenario: A stale frame fails the run
- **WHEN** any action leads to a `QA-STALE` report
- **THEN** the run records a finding with the action that preceded it and a screenshot, and exits non-zero

#### Scenario: A crash fails the run
- **WHEN** the application exits during a run
- **THEN** the run records a crash finding naming the last action

### Requirement: The scenario suite covers every reachable control

The local scenario suite SHALL at least: open every menu bar menu and hover every enabled row; open and dismiss every quick-open overlay; switch to every built-in sidebar view; hover and click every editor tab, panel tab and status segment; drag the sidebar, panel and editor-split dividers; split the editor and close the split; open, type in and close the terminal panel; scroll the editor and the sidebar; and do each of these with the editor, the tree and the terminal holding focus in turn.

#### Scenario: Today's Edit-menu bug is caught
- **WHEN** the suite opens the Edit menu and a caret blink repaints the editor under it with the bounding-box skip that caused the bug
- **THEN** the redraw verifier reports `QA-STALE` and the run fails

### Requirement: Every keybinding reaches the contexts it claims

A check SHALL press every built-in keybinding from every focus target -- editor, tree, search view, git view, output panel, terminal, debug pane, an open quick-open overlay -- and SHALL require that it dispatches its action exactly where its registered context and terminal-skip setting say it is active, and nowhere else. It SHALL run in-process in `microide_tests`; the driver MAY repeat it against the live window.

#### Scenario: A workbench shortcut that only works from the editor fails
- **WHEN** a Global binding (for example find in files) does not dispatch with the tree focused
- **THEN** the check names the binding and the focus target

#### Scenario: A focused terminal keeps its keys
- **WHEN** a Global binding that does not skip the terminal is pressed in a focused terminal
- **THEN** it does not dispatch, and the check fails if it does

### Requirement: The monkey is seeded and replayable

The driver SHALL offer a monkey mode that performs weighted random actions over the named elements and free pointer positions from a given seed, with every oracle armed, and on a finding SHALL save the exact action list so that replaying it on the same build reproduces the finding.

#### Scenario: A finding is replayed
- **WHEN** the monkey with seed S records a finding after N actions
- **THEN** the saved action file replays those actions and triggers the same finding

### Requirement: Remote scenarios run against a real host

The driver SHALL provide scenarios against an ssh host named by `MQA_REMOTE_HOST`: open a host folder and wait for the mirror; change files on the host under open and unopened tabs; save over a host change and resolve the conflict; drop the connection and restore it; open a host terminal and type in it; and run simulated agents writing the tree while menus, project search and an editor split are open -- with every oracle armed, and the mirror byte-identical to the host at the end.

#### Scenario: Agents churn while the UI is busy
- **WHEN** simulated agents write the host tree while the Edit menu and the search view are open
- **THEN** no `QA-STALE`, crash or sanitizer finding is reported and the mirror matches the host

### Requirement: Findings land as unit tests

A QA finding that is fixed SHALL land with a regression test in `microide_tests` or `microide_kernel_tests` that fails without the fix; a finding not fixed SHALL be filed in `dev-docs/project/known-tech-debt.md` with its replay file or scenario name.

#### Scenario: A fix without a unit test is incomplete
- **WHEN** a QA finding is fixed
- **THEN** the fix commit contains a unit test that fails on the parent commit
