# tools/qa — drive the real window, check it, keep the evidence

The unit suite paints frames and sends keys from one focus at a time; a bug that
needs the real event loop, real input from every focus, or the retained scene's
partial frames is invisible to it. `tools/qa` drives the shipped binary the way a
person does — on a private Xvfb display, with real pointer and keyboard input —
and checks it with oracles that need no expected screenshot.

## Setup

```bash
sudo apt install xvfb xdotool x11-apps x11-utils python3-pil   # Xvfb, xdotool, xwd, xdpyinfo, Pillow
sudo apt install clangd gdb                                     # the lsp-* and dbg-* flows
cmake --build build --target microide -j8
```

The `lsp-*` / `outline` flows drive the real clangd through `plugins/cpp-lsp` and
`dbg-session` drives gdb through `plugins/gdb-dap` (seeded into the instance's
config, `SCENARIO_PLUGINS`). A missing tool is a finding, not a skip, unless
`MQA_ALLOW_MISSING_TOOLS=1`.

Any build works, including the sanitizer builds (`--binary build/microide-asan/microide/microide`).

## Running

```bash
tools/qa/run.py suite -j 8                  # every local scenario, 8 at a time (~5 min)
tools/qa/run.py suite menus chrome          # some of them
tools/qa/run.py suite 'edit-*' 'lsp-*'      # by glob
tools/qa/run.py suite --list                # the catalog, one line each
tools/qa/run.py suite menu-clicks           # click every menu row in each layout (dry run)
tools/qa/run.py monkey --seed 1 --steps 300 # the seeded monkey
tools/qa/run.py monkey --profile commands --seeds 1-8 --binary build/microide-asan/microide/microide
                                            # the control-command fuzzer (formerly tools/fuzz-control-commands.py)
tools/qa/run.py replay /tmp/mqa-out-.../seed-1/finding-01-.../replay.json
tools/qa/run.py invariants [--only VERB] [--soft-wrap]
                                            # algebraic invariants of every editing command
                                            # (undo/redo/inverse/idempotent/read-only), on disk
MQA_REMOTE_HOST=worky MQA_REMOTE_ROOT=/home/me/repo tools/qa/run.py remote
tools/run-checks.sh qa                      # the lane: build, suite, 300-step monkey (seed 1)
```

Exit status is non-zero on any finding; the summary names each finding's
directory. `--out DIR` chooses where findings land (default `/tmp/mqa-out-<time>`).

Every run gets its own Xvfb (`-displayfd`, so two drivers can run at once), its own
XDG config/state/data/runtime directories, and a fresh copy of the fixture
(`tools/capture-media/make-fixture.sh`'s `taskflow`), so a seed means the same
actions on the same state and nothing touches your real configuration.

## The scenarios

| group | scenarios | what they check |
|-------|-----------|-----------------|
| chrome | `menus`, `menu-clicks`, `overlays`, `overlay-backdrop`, `sidebar-views`, `chrome`, `dividers`, `split-and-close`, `terminal`, `scrolling` | every chrome surface opened, hovered, clicked from every focus; no stale paint |
| editing | `edit-type-save`, `edit-selection-clipboard`, `edit-multi-cursor`, `edit-line-ops`, `edit-auto-close`, `edit-folding`, `edit-find-replace`, `edit-goto-line`, `edit-unsaved-prompt`, `edit-external-change`, `edit-wrap-scroll` | each edit's text, carets, selections and folds (`buffer`), the file on disk, undo/redo |
| workbench | `wb-quick-open`, `wb-command-palette`, `wb-tree`, `wb-tabs`, `wb-editor-groups`, `wb-project-search`, `wb-git`, `wb-settings`, `wb-status-bar`, `wb-terminal`, `wb-layouts`, `wb-zoom-chrome`, `wb-help` | whole tasks, checked against `ui`/`editor`/`terminals`, the disk and the git repository |
| tools | `lsp-hover`, `lsp-navigation`, `lsp-completion`, `lsp-diagnostics`, `lsp-rename`, `lsp-format`, `outline`, `dbg-session` | clangd and gdb end to end |
| sweeps | `keys`, `context-menus` | every keybinding through the real keyboard in every context; every row of every right-click menu -- each checked by the action receipt |
| session | `session-restore`, `untitled-save-as`, `unicode-editing`, `drag-drop-text`, `terminal-find`, `projects`, `compare-merge` | quit and relaunch, first save, non-ASCII text, several projects, a real conflicting merge |
| advanced | `lsp-signature-help`, `lsp-code-actions`, `snippets`, `sticky-scroll`, `themes`, `tab-overflow`, `dbg-conditions` | parameter hints, quick fixes, snippet placeholders, pinned scopes, every colour scheme, a crowded tab strip, conditional breakpoints |
| coverage | `edit-transforms`, `save-transforms`, `settings-scopes`, `dbg-stepping`, `git-branches`, `lsp-more`, `compare-clipboard`, `tab-reopen` | sort/join/block comment/bracket jump/JSON format/column select, trim and final newline on save, user vs project settings across projects and a restart, a debugger driven from its toolbar and pane (call stack, variables, watch, restart), branches and the stash, type definition/workspace symbols/next problem/call hierarchy, compare with clipboard, Reopen Closed Editor |
| ux | `nav-history`, `caret-motion`, `indentation`, `find-options`, `git-discard`, `terminal-clipboard`, `notifications`, `tree-keys`, `quick-open`, `undo-semantics`, `mouse-and-wrap` | Go Back / Go Forward through files and far jumps (menu state, a closed file reopened, the terminal keeping Alt+Left), Home/End/word/page/document motion, Ctrl+Down scrolling, Ctrl+L and Expand/Shrink Selection, Tab/Shift+Tab over lines, Enter after a brace, Backspace in an indent, whole word/regex by chord and click, Ctrl+H from the find field, a `$1` replace, the query seeded from the word under the caret; untracked files, Stage/Unstage, Discard with Cancel and confirm (disk and open editor), Stage All; editor to shell by Ctrl+Shift+V and the context menu, a terminal row dragged and Ctrl+Shift+C'd into an editor; toasts named, clicked away, stacked, and reached by Focus Notifications; the tree following the active file, type-to-select, F2 rename, Delete with confirm, New Folder, Collapse All; Ctrl+P `file:line`, `>` and `:` prefixes, click to open; undo restoring the replaced selection, one step per multi-caret edit, paste and line move, the caret brought back, redo dropped by a new edit; Shift+wheel sideways, Alt+Z wrap with Down and a double End, a double-click on the tab strip's empty space |

The flows live in `flows_editing.py`, `flows_workbench.py`, `flows_lsp_debug.py`,
`flows_keys.py`, `flows_context_menus.py`, `flows_more.py`, `flows_advanced.py`,
`flows_coverage.py` and `flows_ux.py`; `scenarios.py` holds the chrome scenarios and the registry
(`SCENARIOS`, `SCENARIO_ENV`, `SCENARIO_PLUGINS`). A scenario's plugins are bundled
ones (`plugins/<name>`) or QA-only ones under `plugins/` here, for test content no
bundled plugin ships (`qa-snippets`).

## What a flow reads

- **`buffer`** -- the active buffer's text, every caret with its anchor (primary
  first, 1-based), the selected text, collapsed and foldable ranges, its flags.
- **`ui`** -- named elements with rects (menus and their rows, tabs, tree / search /
  git rows, overlay rows, text fields with their text, prompt and Settings
  controls, the find widget's buttons), plus `overlay` (query, count, selection),
  `prompt`, `popups` (hover text and kind, signature help), `editors` (per pane:
  which buffer line each visible row shows, text x, char width, line height, the
  gutter and fold-marker x -- so `App.click_text(line, column)` lands exactly) and
  the action receipt `actions`. The debug toolbar's buttons
  (`button:debug-toolbar/step-into`), the debug pane's mode tabs (`debug-tab:watch`)
  and its visible rows (`debug-row:callstack/1`, labelled as painted) are named too.
- **`output`** (`App.output(channel)`) -- an output channel's lines: references,
  call hierarchy, workspace symbols, the debug console, the logs.
- **`editor`, `terminals`, `terminal-output`, `keybindings`, `debug-state`,
  `breakpoints`, `notifications`** -- see `dev-docs/control/control-channel.md`.

`App.expect(condition, message)` returns the condition, so a flow gates its next
steps on it; a helper that returned `None` once made a whole flow return early
and pass without checking anything.

## Receipts and dry runs

A press can be taken by the wrong surface with every frame still correct, so the
sweeps check what RAN: `menu.lastRun`/`rowsRun` for menu-bar rows, and
`actions: {dispatched, last, source, rejected}` for any action a key, menu row,
context-menu row or command dispatched (`last` is the command name; menu rows and
keybindings carry theirs as `command`). `MICROIDE_QA_MENU_DRY_RUN=1` makes menu-bar
rows record and not run; `MICROIDE_QA_ACTION_DRY_RUN=1` makes every key-, menu-
and pointer-driven action record and not run (control commands still run, so a
sweep drives with them). `keys` and `context-menus` run with the latter.

## Driver notes (things the window taught the harness)

- Two presses within SDL's double-click time and radius are one double click;
  `click`/`drag` wait the window out before an unrelated press.
- A few pixels at the window's edges are the borderless window's resize grips,
  which SDL takes before the app: click "outside" a modal well inside the window.
- xdotool types '\n' as the Linefeed keysym, which SDL drops: `type` presses
  Return and Tab for '\n' and '\t'. Latin-1 goes as its own keysym press; past it
  Xvfb has no input method, so `type` inserts it through the control channel.
- A hover card is a surface: a press on it is the card's. Move off a hover
  before clicking text it may cover.
- `App.restart()` quits the way a person does and starts again on the same
  display, XDG tree and project (`session-restore`).

## The oracles

Checked after every action:

- **The redraw verifier.** The app runs with `MICROIDE_QA_VERIFY_REDRAW=1`: after
  every partial frame it renders the same prepared frame in full into a second
  target, diffs the two, and logs `QA-STALE frame=N reason=R pixels=P bbox=x,y,w,h
  first=x,y clips=...` when they differ. That is stale paint — a missed damage rect,
  a wrong surface skip, a frame whose renders read different state — with no
  reference image.
- **Sanitizer and allocator reports** (`ERROR: AddressSanitizer`, `runtime error:`,
  `WARNING: ThreadSanitizer`, glibc's `malloc():` / `double free`), assertions, and
  the process exiting.
- **Scenario expectations**, stated against the `ui` query (`--query ui`: every
  on-screen element by name with its rect, plus focus, modal, sidebar, overlay, menu
  and panel state; see `dev-docs/control/control-channel.md`) — never against pixels.

### Clicks reach what they aim at

The redraw verifier checks what is painted; it cannot see a press that went to the
wrong surface. A menu row drawn over the sidebar's view buttons used to switch the
sidebar view instead of running (the view buttons were hit-tested before the open
menu), and every frame stayed correct. Two checks cover that class:

- `menus` asserts that hovering a row highlights that row (`active` on its element):
  the hit test and the painted rows agree.
- `menu-clicks` clicks every enabled row of every menu, and the sidebar's views
  menu, in each sidebar view and in two layouts (default, and the project strip
  shown, which moves the sidebar's controls under different rows), and asserts the
  `ui` receipt (`menu.lastRun`, `menu.rowsRun`) names the row clicked and the menu
  closed. It runs the app with `MICROIDE_QA_MENU_DRY_RUN=1` (`SCENARIO_ENV` in
  `scenarios.py`), so rows record their run without quitting or opening pickers.

A new popup surface (a context menu, a picker) gets the same treatment: a receipt
in `ui` for what a click ran, and a scenario that clicks every row in more than one
layout. Hovering alone does not find a press routed elsewhere.

## Reading a finding directory

```
finding-03-sidebar--click-status-indent/
  findings.txt                 what fired (the QA-STALE line, the report, the expectation)
  trace.txt                    the app's log since the previous action: one
                               `microide redraw frame: frame=N reason=... clips=...` line
                               per frame (MICROIDE_TRACE_REDRAW_VERBOSE), per-clip menu
                               lines, and the QA-STALE lines
  frame-N-retained.png         the verifier's images of stale frame N: what the window
  frame-N-reference.png        showed, and what a full render of the same frame paints
  after-action-NNNN.png        the window after each of the last three actions
  now.png                      the window when the finding was recorded
  replay.json                  the actions up to here; `run.py replay` re-runs them on a
                               fresh fixture
```

Read `findings.txt`, find the frame in `trace.txt` (its reason and clips say which
damage produced it), then diff the two `frame-N-*.png` images — the difference is
the bug. A finding that does not replay still has its frames.

## Turning a finding into a fix

The harness finds; the unit suite pins. Reproduce the finding in-process first —
`tests/support/RetainedRedraw.h` has `DispatchEvents` (events folded as the app loop
folds them) and `PaintAndDiffWithFullRepaint` (retained paint vs a full repaint on
the same canvas) — make it fail, fix, and land the test with the fix. A finding you
do not fix goes into `dev-docs/project/known-tech-debt.md` with its replay file or
scenario name.

## Adding a scenario

A scenario is a function `f(app)` registered in a `FLOWS`/`SCENARIOS` dict that
`scenarios.py` merges. Drive by name (`app.click_named("menu:Edit")`,
`app.hover_named(...)`, `app.click_text(line, col)`, `app.key("ctrl+shift+f")`,
`app.send("open", "src/main.cpp")`; `flows_common.open_file` opens and waits),
run the oracles after each action (`flows_common.step` or `app.check(label)`), and
state expectations with `app.expect(...)`, `app.wait_until(...)` and
`flows_common.expect_text(...)` against what the app reports -- never `sleep` for a
state the app reports, and say in the expectation what VS Code does when the two
could differ. Before writing a fix for a finding, check the expectation: about a
third of first-run findings in the 2026-10-16 pass were the flow's own
assumptions (a column off by one, a key a focused terminal owns).

## Files

| file | what |
|------|------|
| `microide_qa.py` | `App`: lifecycle, input, control channel, `ui` locators, oracles, evidence, replay |
| `scenarios.py` | the chrome scenarios and the registry (`SCENARIOS`, `SCENARIO_ENV`, `SCENARIO_PLUGINS`) |
| `flows_common.py` | open/write/read helpers, `expect_text`, `step`, `dismiss`, `indent_unit` |
| `flows_editing.py`, `flows_workbench.py`, `flows_lsp_debug.py`, `flows_keys.py`, `flows_context_menus.py`, `flows_more.py`, `flows_advanced.py`, `flows_coverage.py`, `flows_ux.py` | the flows, by group |
| `plugins/` | QA-only plugins a scenario seeds (`qa-snippets`) |
| `monkey.py` | the seeded monkey and the command fuzzer's generators, profiles and churn |
| `invariants.py` | the editor-invariants sweep (formerly `tools/sweep-editor-invariants.py`) |
| `run.py` | the CLI |
| `remote.py`, `agents.py` | the remote-project scenarios against a real ssh host (on demand) |
