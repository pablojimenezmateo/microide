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

Any build works, including the sanitizer builds (`--binary build/microide-asan/microide/microide`);
set `MQA_SLOW_FACTOR=3` for an instrumented build (coverage, the sanitizers) so every
control timeout stretches with it.

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
eval "$(tools/qa/local-sshd.sh start)" && tools/qa/remote.py   # the same against a loopback sshd
tools/run-checks.sh qa-remote               # that, as a lane (builds the app and the static server)
tools/run-checks.sh qa-asan                 # the scenario suite on the ASan build
tools/run-checks.sh qa                      # the lane: build, suite, 300-step monkey (seed 1)
MQA_BACKEND=wayland tools/qa/run.py suite -j 4   # the same scenarios on a headless weston
MQA_BACKEND=wayland MQA_WAYLAND_SCALE=2 tools/qa/run.py suite   # ... on a HiDPI output
tools/run-checks.sh qa-wayland              # the suite on weston at scale 2, as a lane
```

Exit status is non-zero on any finding; the summary names each finding's
directory. A scenario whose only findings are failed expectations (a step that
timed out, a key that did not land on a loaded box) is run again once, alone,
after the parallel pass (`--retries`, `$MQA_RETRIES`, default 1): a pass then is
reported as `flaky`, with the first run's findings kept above it. A crash, stale
paint or a sanitizer report is never retried. `--out DIR` chooses where findings land (default `/tmp/mqa-out-<time>`).

Every run gets its own Xvfb (`-displayfd`, so two drivers can run at once), its own
XDG config/state/data/runtime directories, and a fresh copy of the fixture
(`tools/capture-media/make-fixture.sh`'s `taskflow`), so a seed means the same
actions on the same state and nothing touches your real configuration.

### The Wayland backend

`MQA_BACKEND=wayland` runs each instance under its own headless weston (kiosk
shell, `--scale` from `MQA_WAYLAND_SCALE`, a socket under the run's runtime
directory) with `SDL_VIDEODRIVER=wayland`. A Wayland compositor takes input from no
other client, so the driver's mouse and keyboard go through the control channel
instead of xdotool: `pointer [mods+]<move|down|up|click|wheel> x y ...` and `key
<chord>` (which types a printable key's character, as a keyboard does). The input
enters at SDL's event level, not the seat: what that path cannot show -- the
compositor's keyboard focus, a clipboard serial -- stays X11's to cover.
Screenshots are blank there (no screen reader without weston's debug protocols);
the redraw verifier, which renders in process, is what checks the paint.
`tools/qa/weston.sh` unpacks weston from the distribution's package into
`~/.cache/microide-qa/weston` when it is not installed; nothing is installed and no
session is touched.

## The scenarios

| group | scenarios | what they check |
|-------|-----------|-----------------|
| chrome | `menus`, `menu-clicks`, `overlays`, `overlay-backdrop`, `sidebar-views`, `chrome`, `dividers`, `split-and-close`, `terminal`, `scrolling` | every chrome surface opened, hovered, clicked from every focus; no stale paint |
| editing | `edit-type-save`, `edit-selection-clipboard`, `edit-multi-cursor`, `edit-line-ops`, `edit-auto-close`, `edit-folding`, `edit-find-replace`, `edit-goto-line`, `edit-unsaved-prompt`, `edit-external-change`, `edit-wrap-scroll` | each edit's text, carets, selections and folds (`buffer`), the file on disk, undo/redo |
| workbench | `wb-quick-open`, `wb-command-palette`, `wb-tree`, `wb-tabs`, `wb-editor-groups`, `wb-project-search`, `wb-git`, `wb-settings`, `wb-status-bar`, `wb-terminal`, `wb-layouts`, `wb-zoom-chrome`, `wb-help` | whole tasks, checked against `ui`/`editor`/`terminals`, the disk and the git repository |
| tools | `lsp-hover`, `lsp-navigation`, `lsp-completion`, `lsp-diagnostics`, `lsp-rename`, `lsp-format`, `outline`, `dbg-session` | clangd and gdb end to end |
| sweeps | `keys`, `context-menus` | every keybinding through the real keyboard in every context; every row of every right-click menu -- each checked by the action receipt |
| session | `session-restore`, `untitled-save-as`, `unicode-editing`, `drag-drop-text`, `terminal-find`, `projects`, `compare-merge` | quit and relaunch, first save, non-ASCII text, several projects, a real conflicting merge |
| advanced | `lsp-signature-help`, `lsp-code-actions`, `snippets`, `snippets-deep`, `sticky-scroll`, `themes`, `tab-overflow`, `dbg-conditions` | parameter hints, quick fixes, snippet placeholders (and the engine by keyboard: Tab on a prefix, mirrors, choices, nesting, variables, one undo, a snippet completion item), pinned scopes, every colour scheme, a crowded tab strip, conditional breakpoints |
| coverage | `edit-transforms`, `save-transforms`, `settings-scopes`, `dbg-stepping`, `git-branches`, `lsp-more`, `compare-clipboard`, `tab-reopen` | sort/join/block comment/bracket jump/JSON format/column select, trim and final newline on save, user vs project settings across projects and a restart, a debugger driven from its toolbar and pane (call stack, variables, watch, restart), branches and the stash, type definition/workspace symbols/next problem/call hierarchy, compare with clipboard, Reopen Closed Editor |
| depth | `terminal-programs`, `terminal-escapes`, `unsaved-continuations`, `branch-review`, `selected-ranges`, `merge-mouse`, `compare-mouse`, `save-pipeline` | paths a coverage build of the suite never ran (TD-2026-10-18-364): SGR colours (16, 256, truecolour), an OSC title naming the pane, `less` paging on the alternate screen and quitting back, `vim` writing a file, a click reaching a program under mouse reporting as `ESC [ M`; escape sequences one by one, read back from the screen (cursor motion and overwrite, erase in line and display, DECSC/DECRC, insert line, a scroll region, tab stops, the cursor hidden and shown, an OSC 8 hyperlink, bracketed paste reaching `cat -v` as its markers); the unsaved prompt before a rename (Cancel returns to the name typed), Close All Tabs (Save all writes both), Close Project (Discard leaves the disk alone), and a quit as a hot exit (no prompt, the edit back unsaved after a relaunch); an Outgoing file marked reviewed showing its marker on the git row, keeping it across a restart, and clearing (the marks refused outside such a diff); Ctrl+K Ctrl+Alt+S staging only the change at the caret of a working-tree diff, and Ctrl+K Ctrl+R asking before reverting the other; the maximized terminal (Ctrl+Shift+J: the shell sees its size, `less` fills it, the restore gives the rows back); two merge conflicts resolved by the merge editor's own buttons and saved; a diff by mouse (the divider dragged and double-clicked back, a collapsed block's Show next / previous / all, click, double-, triple-click and drag on the working side); a plugin's save participant and formatter (`qa-save`) on Ctrl+S, with and without format on save |
| resilience | `crash-recovery`, `big-repo`, `huge-file`, `state-dump` | a SIGKILL mid-edit and the hot-exit restore after it; a 10k-file tree (quick open, project search, the tree) and a buffer past the 1 MiB text cap staying responsive; `microide dump-state` decoding every persisted file a session wrote (and refusing a file that is not one) |
| matrix | `sweeps-matrix` | `keys` and `context-menus` again under each layout the default run never sees: the project strip, compact mode, ui.scale 1.5 with no status bar and file icons, the light theme with a 20pt editor font |
| integration | `file-drop`, `control-spec`, `dbg-function-breakpoint` | a project file, an outside file, a folder and a missing path dropped on the window (`pointer drop`); a cold start from `--control --control-spec` (transient settings, files, a function breakpoint, commands, one `applied` line each); a breakpoint by function name |
| plugins | `plugin-api` | one of each plugin provider and event (`qa-api`), each reached as a person would: commands from the palette, a menu, a keybinding and the control channel; hover, completion, code actions, definition, references, signature help, document symbols, a tree sidebar, tests, the buffer events -- the plugin counts each callback |
| settings | `settings-sweep`, `settings-controls`, `settings-persist` | every registered setting (from the `settings` query, so a new one is swept the day it lands) set to every value it can take -- both booleans, every enum value, the ends of a numeric range, a harmless string -- each read back, shown by its Settings row, and with a receipt where one is cheap (the status bar leaving the layout, the line height, the window scale, the buffer's tab size); a wrong value refused and a value past the range clamped; each kind of row control by pointer (checkbox, a stepper held at both bounds, a segmented enum walked and wrapped, a text value edited and an edit abandoned, the font picker filtered and picked from, Reset, the filter by description and by nothing, the wheel); user- and project-scope values read back after a restart, each from its own config file |
| inputs | `native-dialogs`, `menu-keyboard`, `prompts-keyboard`, `tab-drag`, `tree-file-ops`, `terminal-selection`, `editorconfig` | File > Open Folder… / Open File… and the font row's Choose file… through the desktop dialog (SDL's zenity driver with `fakebin/zenity` first on PATH: the flow writes the answer, so the app's own result handling runs for a selection, a cancel and a failure; with no dialog program the row falls back to the palette seeded with its verb); an open menu by keys (Left/Right/Tab between menus, Down/Up/Home/End over the rows skipping disabled ones, Enter running the row, Escape), the compact layout's chevron popup by keys, the tree's context menu by keys; the unsaved prompt by S/D/C, arrows and Enter, Escape, a confirm prompt by Escape and Enter, and a text prompt's field with the single-line keys (Home/End, Ctrl+arrows, Shift selection, Ctrl+A/C/X/V, Ctrl+Backspace/Delete) and the mouse (a click placing the caret, a double-click selecting the word, a triple-click selecting all, a drag); a tab dragged along its strip, dropped on the editor's edge to split, onto the other strip, and the last tab leaving a group; a rename or delete under an open editor (the tab follows a renamed file and a renamed folder, closes with a deleted file) and the same done outside the app; the terminal's word (double-click), line (triple-click) and two-line (drag) selections copied and pasted back; a `.editorconfig` with sections for spaces, tabs, CRLF, `unset` and a nested root, and the master switch |
| surfaces | `commands-args`, `search-view-keys`, `panel-mouse`, `overview-ruler`, `diff-wrap` | every argument-taking command with an argument and its effect read back (goto/jump with columns and signs, tab size and soft tabs, wrap, a colour scheme and an unknown one refused, open/tab/tabswitch/tabmove/split/focus-group/tab-to-group, sidebar show/toggle/width, the finder and search seeded, `term`, `show-output`, ui-scale, compare-files, merge, review-commit, save-as by path, format-json, git-stash and a branch switch, a breakpoint set and cleared); the search view by keys (Down/Up/j/k/Home/End, Enter opening the result at its line and seeding the find widget, `/` `=` `i` `x` opening a field without typing into it, Tab cycling the fields, `c` recounting); the panel's tabs clicked, closed by button and by middle-click, the panel divider dragged and double-clicked back, a split's pane divider dragged and double-clicked back, maximize on and off; the overview ruler's marks through scrolling and the setting off and on; a diff with long lines wrapped, scrolled and unwrapped |
| panes | `save-wait`, `git-view-keys`, `plugin-sidebar-keys`, `debug-pane-mouse`, `dirty-close-group` | a close that has to save first while the save's formatter still runs (the qa-save formatter's slow mode): Close All Tabs closes the tabs once both writes land, Close Project waits behind a progress row with Cancel; the git view by keys (Down/Up/Home/End over the entries, `s`/`u` staging and unstaging the selected one with `git status` as the receipt, `d` and Enter opening its diff, `R` refreshing); a plugin's tree view by keys (Right/Left/Space on a collapsible row, Enter confirming, R); the debug pane by mouse at a gdb stop (a variable row's context menu: Copy Value, Add to Watch; the Breakpoints rows: a click jumping to the line, the context menu disabling and removing; a variable's value edited in place by double-click, Enter and Escape; the wheel); closing a group whose tab is dirty asks first (Cancel keeps, Discard closes, the disk untouched) and Reload from Disk on a dirty buffer says why it will not |
| effects | `editor-toggles`, `language-overrides` | what a switch DOES, not only that it reads back: each editor behaviour toggle (auto-close, surround, smart indent, the comment and line-operation actions, sorting, Add Cursor at Match, case-sensitive seeds, snippets) run from the palette, its behaviour checked in the buffer before and after; the language contract's user overrides (an extra pair, a disabled pair, the comment marker, an extra indent pattern, a disabled snippet prefix) each set live and shown by typing |
| lifecycle | `plugins-reload` | Reload Plugins twice with a plugin's every contribution counted (keybinding, menu row, status item, sidebar view, commands, completion items: each there once, none lost), the language still detected and the providers answering; then the plugin's source edited on disk and reloaded, and the edit in effect |
| diff | `diff-commands`, `diff-review-files`, `merge-commands` | the diff's own commands with receipts: Ignore Whitespace hiding a whitespace-only change (the `changes` count), Show Whitespace, Copy Path, Copy Change / Copy File as Patch read back by pasting and proven exact by `git apply --check -R`; a three-file commit under review stepped by Next/Previous File (it opens on its first file); the merge editor's Copy Incoming/Current Side (pasted back) and Git: Accept Incoming resolving the whole file, staged |
| debugger | `dbg-breakpoint-kinds`, `dbg-pause`, `dbg-sessions` | a hit count (stops on that pass, read off `task.cost`), a log message (no stop, one console line per pass, kept after the program ends), a breakpoint disabled and enabled, all from the gutter menu; F6 pausing a spinning program (`debug-state.session` to wait for it to run), F5, Shift+F5; two programs at once: Switch Debug Session cycling the active one (F6 then pauses that one only, read off `debug-state.sessions`), Stop All Debug Sessions |
| layout | `groups-move`, `resize-overlays` | Move Editor Group Left/Right/Up/Down, at the edges and across a column, Focus Next Editor Group round three groups, Close Editor Group keeping the others in order and each a pane; the window shrunk to 640x480 and grown back with each overlay, modal, prompt and menu open (the palette, quick open, the find widget, Settings, the shortcuts, a menu, a context menu, the rename prompt): everything inside the window, nothing overlapping, and the surface still taking its keys |
| views | `problems-tests-views` | The Problems view with three problems in two files (published by the `qa-api` plugin, no language server): rows name file and position, Down/Up move the selection, Enter opens each at its line and gives the editor the keyboard, clearing empties the view and the status count; the Tests view lists the discovered test, Enter opens it at its line, `r` runs it and the row shows `passed` |
| tree | `tree-collisions`, `rename-open-tabs` | the tree's operations where they collide: a rename onto a name that exists, a new file or folder over one that exists (each refused by a toast that says why, the disk untouched), a reserved or escaping name (the prompt keeps it for correction and makes nothing), a file made inside directories that do not exist yet, a folder deleted with a file open from it (its tab closes); and a rename under every kind of open tab: a file's diff follows it, a dirty file open in two groups asks first and Save writes the edit under the new name with both tabs following, Discard renames the file as it is on disk and reopens the tab there |
| commands | `commands-run` | every command of the `commands` query run once for real, bare, from a source file, from a diff and with no project (the menu and key sweeps are dry runs); set `MQA_COMMANDS_LOG=1` to print each refusal |
| languages | `lsp-typescript`, `lsp-csharp` | typescript-language-server (from `MQA_TS_NODE_MODULES`, default `~/.cache/microide-qa/ts/node_modules`) and csharp-ls (`~/.dotnet/tools`) through the bundled plugins: hover, F12 across files, a compile error reaching the problems count |
| ux | `nav-history`, `caret-motion`, `indentation`, `find-options`, `git-discard`, `terminal-clipboard`, `notifications`, `tree-keys`, `quick-open`, `undo-semantics`, `mouse-and-wrap`, `search-options`, `groups-keyboard`, `settings-keyboard`, `completion-keys`, `debug-keys`, `terminal-keys`, `recents-and-autosave`, `plugins-ui`, `git-remote`, `compare-keys` | Go Back / Go Forward through files and far jumps (menu state, a closed file reopened, the terminal keeping Alt+Left), Home/End/word/page/document motion, Ctrl+Down scrolling, Ctrl+L and Expand/Shrink Selection, Tab/Shift+Tab over lines, Enter after a brace, Backspace in an indent, whole word/regex by chord and click, Ctrl+H from the find field, a `$1` replace, the query seeded from the word under the caret; untracked files, Stage/Unstage, Discard with Cancel and confirm (disk and open editor), Stage All; editor to shell by Ctrl+Shift+V and the context menu, a terminal row dragged and Ctrl+Shift+C'd into an editor; toasts named, clicked away, stacked, and reached by Focus Notifications; the tree following the active file, type-to-select, F2 rename, Delete with confirm, New Folder, Collapse All; Ctrl+P `file:line`, `>` and `:` prefixes, click to open; undo restoring the replaced selection, one step per multi-caret edit, paste and line move, the caret brought back, redo dropped by a new edit; Shift+wheel sideways, Alt+Z wrap with Down and a double End, a double-click on the tab strip's empty space; Find in Files seeded from the selection, its case/regex toggles, include/exclude globs, Enter on a result; editor groups by keyboard: Ctrl+\\, Ctrl+1/2, the Ctrl+K Ctrl+arrow focus and Ctrl+K Ctrl+Shift+arrow move chords, Ctrl+K Ctrl+0/J, a group closing with its last editor; Settings by keyboard: Space toggles the selected row, Reset, a category jump, Escape, reopening unfiltered; completion by keyboard: Down/Up, Tab to accept, a second Ctrl+Space, the list closing when the caret leaves the word; the debugger by keyboard: F9 on the caret's line, F5 from a plugin's launch configuration and to continue, Ctrl+Shift+F5, Delete in the Breakpoints pane, Shift+F5; the terminal by keyboard: Ctrl+Shift+`, Ctrl+PageUp/PageDown, Shift+PageUp scrollback and the snap back, Alt+Left/Right between split panes, Enter relaunching an exited shell; recent files leading an empty Quick Open, both auto-save modes; the demo plugins' status item (clicked until it updates), decorations, panel surface and colour scheme; Publish Branch, Sync pushing (its label counting what is ahead), Fetch and Pull from another clone through a local bare `origin`, the open editor reloading; a diff by keyboard: opening on its first change, Alt+] / Alt+[, an edit on the working side saved by Ctrl+S, Alt+O to the working file |

The flows live in `flows_*.py`, one module per group; `scenarios.py` holds the
chrome scenarios and the registry. Each module exports `FLOWS` (`{name: f(app)}`)
and may export `PLUGINS` (bundled ones, `plugins/<name>`, or QA-only ones under
`plugins/` here, for test content no bundled plugin ships: `qa-snippets`;
`qa-launch`, a launch configuration for the fixture's binary; `qa-api`, one of
each provider and event) and `ENV`. `scenarios.FLOW_MODULES` lists the modules in
suite order; a `flows_*.py` missing from it, or a scenario name two modules
define, fails the import rather than dropping a flow. Readers several modules
need (`git`, `titles`, `tree_rows`, `terminal_tabs`, `messages`, ...) live in
`flows_common.py`: add one there, not a sixth private copy.

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
- **Stranded focus.** Keyboard focus on a surface that is not shown -- the overlay
  with no overlay, modal or menu up, a hidden sidebar or panel -- is a finding:
  every key would go nowhere (what Escape from the palette did to the file tree).
- **Inside the window.** An element whose rect leaves the window is a finding
  (toasts laid out above it while the status bar was hidden; the Settings card
  centred on a narrow editor area).
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
  menu, in each sidebar view and in four layouts (default; the project strip shown,
  which moves the sidebar's controls under different rows; the compact layout;
  ui.scale 1.5 with no status bar and file icons -- `MENU_CLICK_LAYOUTS`, every
  layout setting every key it uses), and asserts the
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

A scenario is a function `f(app)` in a module's `FLOWS` dict (a new module goes
into `scenarios.FLOW_MODULES`). Drive by name (`app.click_named("menu:Edit")`,
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
| `scenarios.py` | the chrome scenarios and the registry (`FLOW_MODULES` merged into `SCENARIOS`, `SCENARIO_ENV`, `SCENARIO_PLUGINS`) |
| `flows_common.py` | open/write/read helpers, `expect_text`, `step`, `dismiss`, `indent_unit`, the shared readers (`git`, `titles`, `tree_rows`, `terminal_tabs`, `active_root`, `messages`) |
| `flows_*.py` | the flows, one module per group (`scenarios.FLOW_MODULES`) |
| `plugins/` | QA-only plugins a scenario seeds (`qa-snippets`) |
| `fakebin/` | stand-in programs a scenario puts first on the app's PATH (`zenity`, for the native dialogs) |
| `monkey.py` | the seeded monkey and the command fuzzer's generators, profiles and churn |
| `invariants.py` | the editor-invariants sweep (formerly `tools/sweep-editor-invariants.py`) |
| `run.py` | the CLI |
| `remote.py`, `agents.py` | the remote-project scenarios against a real ssh host (on demand) |
| `local-sshd.sh`, `slow-link.py` | a loopback sshd as the current user for `remote.py`, and its `qa-slow` alias: the same host behind a 60 ms each-way link (`MQA_SLOW_LINK_DELAY_MS`) |
| `weston.sh` | unpacks weston for the Wayland backend |
