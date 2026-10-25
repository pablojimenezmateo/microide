# tools/qa — drive the real window, check it, keep the evidence

The unit suite paints frames and sends keys from one focus at a time; a bug that
needs the real event loop, real input from every focus, or the retained scene's
partial frames is invisible to it. `tools/qa` drives the shipped binary the way a
person does — on a private Xvfb display, with real pointer and keyboard input —
and checks it with oracles that need no expected screenshot.

## Setup

```bash
sudo apt install xvfb xdotool x11-apps x11-utils python3-pil   # Xvfb, xdotool, xwd, xdpyinfo, Pillow
sudo apt install clangd gdb nodejs                              # the lsp-* and dbg-* flows
(mkdir -p ~/.cache/microide-qa/ts && cd ~/.cache/microide-qa/ts && npm install typescript@5 typescript-language-server)
dotnet tool install --global csharp-ls                          # lsp-typescript, lsp-csharp
cmake --build build --target microide -j8
```

The `lsp-*` / `outline` flows drive the real clangd through `plugins/cpp-lsp` and
`dbg-session` drives gdb through `plugins/gdb-dap` (seeded into the instance's
config, `SCENARIO_PLUGINS`). A missing tool is a finding, not a skip, unless
`MQA_ALLOW_MISSING_TOOLS=1`.

Any build works, including the sanitizer builds (`--binary build/microide-asan/microide/microide`);
set `MQA_SLOW_FACTOR=3` for an instrumented build (coverage, the sanitizers) so every
control timeout and polling expectation stretches with it. The `qa`, `qa-asan`
and `qa-wayland` lanes first run the driver regression tests (`qa-driver`),
including simulated slow responses and bounded timeout failures.

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
tools/run-checks.sh qa-driver               # driver regression tests without launching a window
tools/run-checks.sh qa                      # the lane: build, suite, 300-step monkey (seed 1)
MQA_BACKEND=wayland tools/qa/run.py suite -j 4   # the same scenarios on a headless weston
MQA_BACKEND=wayland MQA_WAYLAND_SCALE=2 tools/qa/run.py suite   # ... on a HiDPI output
tools/run-checks.sh qa-wayland              # the suite on weston at scale 2, as a lane
```

Startup, fixture and driver exceptions fail their scenario without aborting the
remaining suite or qualifying for an expectation retry. The traceback is kept in
`<scenario>/driver-error.txt`; failed launches also keep their app/compositor logs
under `<scenario>/startup/` before the private work tree is removed.

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
| editing | `edit-type-save`, `edit-selection-clipboard`, `edit-multi-cursor`, `edit-multi-caret-lines`, `edit-line-ops`, `edit-auto-close`, `edit-folding`, `edit-find-replace`, `edit-goto-line`, `edit-unsaved-prompt`, `edit-external-change`, `edit-wrap-scroll` | each edit's text, carets, selections and folds (`buffer`), the file on disk, undo/redo; line operations over several carets: Insert Line Above at two carets with their own indents, Delete Line over a selection spanning lines, Tab with a whole-line selection and a point caret, Toggle Block Comment over two selections and back |
| format transactions | `file-format-transactions` | LF/CRLF × UTF-8 with/without BOM: exact bytes across save, undo/redo, restart, and adding a new line after restart; Unicode text and absent final newline preserved |
| replacement matrix | `edit-replace-matrix` | nine literal/regex cases: punctuation and literal dollar signs, adjacent and overlapping candidates, deletion, captures, whole-word/case filters, absent matches and empty buffers; exact match counts, one-step undo/redo, dirty state, focus recovery and saved bytes |
| split lifecycle | `tab-shared-history`, `tab-view-identity`, `tab-inactive-close`, `tab-shared-close` | shared dirty markers and save/undo state across views; independent caret/selection state; Cancel/Save on an inactive close target; no unsaved prompt until the final shared view, then Cancel/Discard with disk receipts |
| search/tree transactions | `tree-cancel-transactions`, `search-replace-scope-transaction`, `search-query-recovery` | cancel rename/create/delete without losing dirty edits or history; cancel then confirm project replacement with include/exclude intersection and clean-editor reload; broad, absent, empty and narrow queries replace stale results, and the final hit opens the correct file/line |
| transactions | `edit-selection-transactions`, `edit-tab-save-transactions`, `file-path-identity-restart` | word, newline, cross-line and whole-buffer replacement/deletion with selection restoration on undo; independent tab histories across save, undo, redo, close and reopen; duplicate basenames and spaces/Unicode/punctuation in paths surviving hot exit with distinct unsaved content and exact disk receipts |
| workbench | `wb-quick-open`, `wb-command-palette`, `wb-tree`, `wb-tabs`, `wb-editor-groups`, `wb-project-search`, `wb-git`, `wb-settings`, `wb-status-bar`, `wb-terminal`, `wb-layouts`, `wb-zoom-chrome`, `wb-help` | whole tasks, checked against `ui`/`editor`/`terminals`, the disk and the git repository |
| tools | `lsp-hover`, `lsp-workspace-edits`, `lsp-navigation`, `lsp-declaration-implementation`, `lsp-completion`, `lsp-diagnostics`, `lsp-rename`, `lsp-format`, `outline`, `goto-symbol`, `dbg-session` | clangd and gdb end to end (Go to Declaration, and a navigation that finds nothing saying so; Go to Symbol in Editor by Ctrl+Shift+O and by `@` in Quick Open, previewing and restored by Escape); a scripted server (the `qa-lsp` plugin) for the workspace edits clangd cannot make without an index: F2 renaming in the open file and on disk in one that is not open (after the Rename Across Files prompt), a code action creating one file and renaming another, a code lens (clicked by its `button:lens/` name) whose server command answers with workspace/applyEdit |
| sweeps | `keys`, `context-menus` | every keybinding through the real keyboard in every context; every row of every right-click menu -- each checked by the action receipt |
| session | `session-restore`, `untitled-save-as`, `unicode-editing`, `drag-drop-text`, `terminal-find`, `projects`, `compare-merge` | quit and relaunch, first save, non-ASCII text, several projects, a real conflicting merge |
| advanced | `lsp-signature-help`, `lsp-code-actions`, `snippets`, `snippets-deep`, `sticky-scroll`, `themes`, `tab-overflow`, `dbg-conditions` | parameter hints, quick fixes, snippet placeholders (and the engine by keyboard: Tab on a prefix, mirrors, choices, nesting, variables, one undo, a snippet completion item), pinned scopes, every colour scheme, a crowded tab strip, conditional breakpoints |
| coverage | `edit-transforms`, `save-transforms`, `settings-scopes`, `dbg-stepping`, `git-branches`, `lsp-more`, `compare-clipboard`, `tab-reopen` | sort/join/block comment/bracket jump/JSON format/column select, trim and final newline on save, user vs project settings across projects and a restart, a debugger driven from its toolbar and pane (call stack, variables, watch, restart), branches and the stash, type definition/workspace symbols/next problem/call hierarchy, compare with clipboard, Reopen Closed Editor |
| depth | `terminal-programs`, `terminal-escapes`, `unsaved-continuations`, `branch-review`, `selected-ranges`, `merge-mouse`, `compare-mouse`, `save-pipeline` | paths a coverage build of the suite never ran (TD-2026-10-18-364): SGR colours (16, 256, truecolour), an OSC title naming the pane, `less` paging on the alternate screen and quitting back, `vim` writing a file, a click reaching a program under mouse reporting as `ESC [ M`; escape sequences one by one, read back from the screen (cursor motion and overwrite, erase in line and display, DECSC/DECRC, insert line, a scroll region, tab stops, the cursor hidden and shown, an OSC 8 hyperlink, bracketed paste reaching `cat -v` as its markers); the unsaved prompt before a rename (Cancel returns to the name typed), Close All Tabs (Save all writes both), Close Project (Discard leaves the disk alone), and a quit as a hot exit (no prompt, the edit back unsaved after a relaunch); an Outgoing file marked reviewed showing its marker on the git row, keeping it across a restart, and clearing (the marks refused outside such a diff); Ctrl+K Ctrl+Alt+S staging only the change at the caret of a working-tree diff, and Ctrl+K Ctrl+R asking before reverting the other; the maximized terminal (Ctrl+Shift+J: the shell sees its size, `less` fills it, the restore gives the rows back); two merge conflicts resolved by the merge editor's own buttons and saved; a diff by mouse (the divider dragged and double-clicked back, a collapsed block's Show next / previous / all, click, double-, triple-click and drag on the working side); a plugin's save participant and formatter (`qa-save`) on Ctrl+S, with and without format on save |
| resilience | `crash-recovery`, `big-repo`, `huge-file`, `state-dump` | a SIGKILL mid-edit and the hot-exit restore after it; a 10k-file tree (quick open, project search, the tree) and a buffer past the 1 MiB text cap staying responsive; `microide dump-state` decoding every persisted file a session wrote (and refusing a file that is not one) |
| matrix | `sweeps-matrix` | `keys` and `context-menus` again under each layout the default run never sees: the project strip, compact mode, ui.scale 1.5 with no status bar and file icons, the light theme with a 20pt editor font |
| integration | `file-drop`, `control-spec` | a project file, an outside file, a folder and a missing path dropped on the window (`pointer drop`); a cold start from `--control --control-spec` (transient settings, files, a function breakpoint, commands, one `applied` line each) |
| plugins | `plugin-api`, `plugin-editor-api`, `plugin-process-api` | one of each plugin provider and event (`qa-api`), each reached as a person would: commands from the palette, a menu, a keybinding and the control channel; hover, completion, code actions, definition, references, signature help, document symbols, a tree sidebar, tests, the buffer events -- the plugin counts each callback; `ctx.editor` from a command (two edits and a caret move as one undo step, an unopened path refused, set_cursor / set_selection, ghost text accepted by Tab, dismissed by Escape and by clear) and `ctx.notify`; `ctx.process.run` (stdout, exit code, stdin, a failing program, a cwd outside the project refused) and `run_async`'s callback |
| settings | `settings-sweep`, `color-theme-pick`, `settings-controls`, `settings-persist` | every registered setting (from the `settings` query, so a new one is swept the day it lands) set to every value it can take -- both booleans, every enum value, the ends of a numeric range, a harmless string -- each read back, shown by its Settings row, and with a receipt where one is cheap (the status bar leaving the layout, the line height, the window scale, the buffer's tab size); a wrong value refused and a value past the range clamped; each kind of row control by pointer (checkbox, a stepper held at both bounds, a segmented enum walked and wrapped, a text value edited and an edit abandoned, the font picker filtered and picked from, Reset, the filter by description and by nothing, the wheel); user- and project-scope values read back after a restart, each from its own config file; the colour-theme pick (Ctrl+K Ctrl+T, View > Color Theme…) previewing on Down, put back by Escape, kept by Enter |
| inputs | `native-dialogs`, `menu-keyboard`, `prompts-keyboard`, `tab-drag`, `tree-file-ops`, `terminal-selection`, `editorconfig` | File > Open Folder… / Open File… and the font row's Choose file… through the desktop dialog (SDL's zenity driver with `fakebin/zenity` first on PATH: the flow writes the answer, so the app's own result handling runs for a selection, a cancel and a failure; with no dialog program the row falls back to the palette seeded with its verb); an open menu by keys (Left/Right/Tab between menus, Down/Up/Home/End over the rows skipping disabled ones, Enter running the row, Escape), the compact layout's chevron popup by keys, the tree's context menu by keys; the unsaved prompt by S/D/C, arrows and Enter, Escape, a confirm prompt by Escape and Enter, and a text prompt's field with the single-line keys (Home/End, Ctrl+arrows, Shift selection, Ctrl+A/C/X/V, Ctrl+Backspace/Delete) and the mouse (a click placing the caret, a double-click selecting the word, a triple-click selecting all, a drag); a tab dragged along its strip, dropped on the editor's edge to split, onto the other strip, and the last tab leaving a group; a rename or delete under an open editor (the tab follows a renamed file and a renamed folder, closes with a deleted file) and the same done outside the app; the terminal's word (double-click), line (triple-click) and two-line (drag) selections copied and pasted back; a `.editorconfig` with sections for spaces, tabs, CRLF, `unset` and a nested root, and the master switch |
| surfaces | `commands-args`, `search-view-keys`, `panel-mouse`, `overview-ruler`, `diff-wrap` | every argument-taking command with an argument and its effect read back (goto/jump with columns and signs, tab size and soft tabs, wrap, a colour scheme and an unknown one refused, open/tab/tabswitch/tabmove/split/focus-group/tab-to-group, sidebar show/toggle/width, the finder and search seeded, `term`, `show-output`, ui-scale, compare-files, merge, review-commit, save-as by path, format-json, git-stash and a branch switch, a breakpoint set and cleared); the search view by keys (Down/Up/Home/End, typing to navigate to the next result whose line starts with the letter, Enter opening the result at its line and seeding the find widget, Ctrl+Shift+F / Ctrl+Shift+H putting the keyboard in the query / replace field, Search: Toggle Search Details showing the scope fields, Tab cycling the four, Search: Toggle Exact Match Count and Search: Refresh rerunning); the panel's tabs clicked, closed by button and by middle-click, the panel divider dragged and double-clicked back, a split's pane divider dragged and double-clicked back, maximize on and off; the overview ruler's marks through scrolling and the setting off and on; a diff with long lines wrapped, scrolled and unwrapped |
| panes | `save-wait`, `git-view-keys`, `git-view-click-keys`, `plugin-sidebar-keys`, `debug-pane-mouse`, `dirty-close-group` | a close that has to save first while the save's formatter still runs (the qa-save formatter's slow mode): Close All Tabs closes the tabs once both writes land, Close Project waits behind a progress row with Cancel; the git view by keys (Down/Up/Home/End over the entries, typing selecting by file name and staging nothing, Shift+F10 opening the row's menu and the arrows choosing Stage / Unstage / Open Changes / Open File with `git status` as the receipt, Git: Commit… opening the message editor, Enter opening the diff, Git: Refresh; a click keeping the keyboard on the list, so Shift+F10's Stage / Unstage act on the clicked entry and no key reaches the file); a plugin's tree view by keys (Right/Left/Space on a collapsible row, Enter confirming, R); the debug pane by mouse at a gdb stop (a variable row's context menu: Copy Value, Add to Watch; the Breakpoints rows: a click jumping to the line, the context menu disabling and removing; a variable's value edited in place by double-click, Enter and Escape; the wheel); closing a group whose tab is dirty asks first (Cancel keeps, Discard closes, the disk untouched) and Reload from Disk on a dirty buffer says why it will not |
| effects | `editor-toggles`, `language-overrides`, `doc-comment-enter` | what a switch DOES, not only that it reads back: each editor behaviour toggle (auto-close, surround, smart indent, the comment and line-operation actions, sorting, Add Cursor at Match, case-sensitive seeds, snippets) run from the palette, its behaviour checked in the buffer before and after; the language contract's user overrides (an extra pair, a disabled pair, the comment marker, an extra indent pattern, a disabled snippet prefix) each set live and shown by typing; Enter continuing a C-family doc comment with ` * ` (after `/**`, after a ` * ` line, at three carets) and leaving a plain `/*` comment, a pointer line and smart indent off alone |
| lifecycle | `plugins-reload`, `plugin-errors` | Reload Plugins twice with a plugin's every contribution counted (keybinding, menu row, status item, sidebar view, commands, completion items: each there once, none lost), the language still detected and the providers answering; then the plugin's source edited on disk and reloaded, and the edit in effect; a plugin that gets its registrations wrong (the `qa-broken` plugin): each malformed completion, menu, keybinding, setting, status item, formatter and code action refused with a message naming what is missing, and a setup that raises shown as a warning with a Show Errors button to the Plugin Errors channel |
| diff | `diff-commands`, `diff-review-files`, `merge-commands`, `merge-toolbar`, `git-outgoing-base`, `diff-hunk-keys`, `merge-panes`, `git-stash-switch` | the diff's own commands with receipts: Ignore Whitespace hiding a whitespace-only change (the `changes` count), Show Whitespace, Copy Path, Copy Change / Copy File as Patch read back by pasting and proven exact by `git apply --check -R`; a three-file commit under review stepped by Next/Previous File (it opens on its first file); the merge editor's Copy Incoming/Current Side (pasted back) and Git: Accept Incoming resolving the whole file, staged; the merge toolbar and keys: Next / Previous, Toggle Base (the band under the toolbar), Unresolved, Alt+R resetting a resolved conflict, Mark Resolved refusing while a conflict is open and staging once both are resolved and saved; the Outgoing section's base dropdown: Auto, Previous commit narrowing to the last commit's files, and the ref picker walked by Up/Down with main taken by Enter; a diff's hunk keys on its read-only side: ] to a change, `a` staging it alone, `c` unstaging, j / k, `d` asking before discarding just that hunk; the merge editor's panes on a long file: a divider dragged and put back by a double-click, the wheel bringing a conflict below the fold into view and back, the current side's accept button and the result's Base; Stash Changes and Pop Stash (the file and its open buffer follow), Switch Branch from the picker and back |
| debugger | `dbg-breakpoint-kinds`, `dbg-pause`, `dbg-sessions`, `dbg-function-breakpoints`, `dbg-exception-breakpoints`, `dbg-commands` | a hit count (stops on that pass, read off `task.cost`), a log message (no stop, one console line per pass, kept after the program ends), a breakpoint disabled and enabled, all from the gutter menu; F6 pausing a spinning program (`debug-state.session` to wait for it to run), F5, Shift+F5; two programs at once: Switch Debug Session cycling the active one (F6 then pauses that one only, read off `debug-state.sessions`), Stop All Debug Sessions; a function breakpoint from the palette's prompt (stops in the function, passed once toggled off, a condition stopping once of four calls); the adapter's exception filters from the Breakpoints view ("when thrown" stops inside the throw, "when caught" at the catch, neither passes), still listed between sessions, a condition per filter set where it takes one and refused where it does not; Enable Debugger, breakpoint conditions / hit counts / log messages through the control API, Select Launch Configuration, Show Call Stack, Show Debug Output, the pane toggled, Step Back and Reverse Continue refused on gdb, debug-launch and debug-start |
| layout | `groups-move`, `resize-overlays` | Move Editor Group Left/Right/Up/Down, at the edges and across a column, Focus Next Editor Group round three groups, Close Editor Group keeping the others in order and each a pane; the window shrunk to 640x480 and grown back with each overlay, modal, prompt and menu open (the palette, quick open, the find widget, Settings, the shortcuts, a menu, a context menu, the rename prompt): everything inside the window, nothing overlapping, and the surface still taking its keys |
| views | `problems-tests-views` | The Problems view with three problems in two files (published by the `qa-api` plugin, no language server): rows name file and position, Down/Up move the selection, Enter opens each at its line and gives the editor the keyboard, clearing empties the view and the status count; the Tests view lists the discovered test, Enter opens it at its line, `r` runs it and the row shows `passed` |
| scrollbars | `scrollbars` | every scrollbar by its `scrollbar:<surface>` element (track plus thumb): the editor's track clicked below the thumb, both editor thumbs, the tree's, the terminal scrollback's, the command palette's and Settings', each dragged to its end and back; scrolling never moves the caret |
| pickers | `argument-pickers` | commands that need an argument run bare from the palette: Show Output Channel… a pick of the channels (Enter shows one), Move Tab to Group a pick of the other pane (Enter moves the tab), Compare Files… over an open file the finder titled for it (its pick compared) |
| startup | `startup-focus` | a fresh launch: the project's terminal open in the panel, the keyboard in the editor, the first typed letter landing in the file |
| tree | `tree-collisions`, `rename-open-tabs`, `tree-clipboard`, `tree-drag` | the tree's operations where they collide: a rename onto a name that exists, a new file or folder over one that exists (each refused by a toast that says why, the disk untouched), a reserved or escaping name (the prompt keeps it for correction and makes nothing), a file made inside directories that do not exist yet, a folder deleted with a file open from it (its tab closes); and a rename under every kind of open tab: a file's diff follows it, a dirty file open in two groups asks first and Save writes the edit under the new name with both tabs following, Discard renames the file as it is on disk and reopens the tab there; the tree's clipboard by Ctrl+C/X/V and by the row menu (a copy beside its source named `a copy.txt` then `a copy 2.txt`, into a folder under its own name, a cut dirty file moving with its tab and unsaved text, a folder refused moving into itself); a tree row dragged onto a pane's right edge (a new pane with the file), a pane's tab strip (opened there), a folder with Ctrl (copied) and without (moved, its tab following), and the terminal (its quoted path typed at the prompt) |
| commands | `commands-run` | every command of the `commands` query run once for real, bare, from a source file, from a diff and with no project (the menu and key sweeps are dry runs); set `MQA_COMMANDS_LOG=1` to print each refusal |
| languages | `lsp-typescript`, `lsp-csharp` | typescript-language-server (from `MQA_TS_NODE_MODULES`, default `~/.cache/microide-qa/ts/node_modules`) and csharp-ls (`~/.dotnet/tools`) through the bundled plugins: hover, F12 across files, a compile error reaching the problems count |
| files | `file-formats`, `file-vanish`, `undo-save-point`, `untitled-numbering`, `save-all`, `revert-file` | files as they are on disk, checked against the bytes: indentation detected per file and kept after a tab switch and a settings change, detection off by its toggle; a CRLF file staying CRLF with its new line; a BOM kept; a binary file's other bytes written back exactly; a dirty buffer of a deleted file saved back with no banner, a clean one keeping its tab, a changed one refused and overwritten from the banner; dirty and clean across undo and redo past a save; Untitled-1/-2, the prompt naming its buffer, the freed number reused, Save As; File > Save All and Ctrl+K S over two groups and an untitled buffer (both files written, the untitled one left and named in the notice, no S typed, the row greyed out once nothing is unsaved, a file changed on disk refused by name while the other is written); File > Revert File taking the disk's text as one undoable edit (both panes of a split, Ctrl+Z bringing the changes back dirty, Ctrl+Y clean again, the row greyed out and the command refused on a clean file, a changed-on-disk banner answered by it) |
| tabs | `tab-context-menu`, `tab-open-twice`, `tab-recent-editors` | the tab menu run for real: Close to the Right / Left relative to the tab right-clicked, Close Others asking for a dirty tab, both Copy Path rows pasted back, Reveal in File Tree into a collapsed tree, Select for Compare then Compare with Selected, Close All; an open file activated rather than reopened, Quick Open into the focused group, one buffer in two groups; Ctrl+Tab in use order (tap, tap back, Ctrl held over one and two Tabs, Escape under Ctrl) and F6 / Shift+F6 between parts |
| editor views | `view-toggles`, `column-select`, `previous-navigation` | the paint-only switches (whitespace, indent guides, bracket match, occurrences) each change the editor's pixels and toggle back to the same frame; sticky scroll pinning a header; Toggle Fold at Cursor and folding off; column selection by Ctrl+Shift+Alt+arrows and Ctrl+Alt+Up; Shift+F3 wrapping, Shift+Alt+F8 and Shift+F8 backwards |
| review | `review-marks`, `git-unstage-and-push` | a hunk marked reviewed drawn on the diff and counted (`reviewedHunks`), a note counted, the file mark on the git row, Review Diff Against Ref; Ctrl+K Ctrl+N unstaging only the change at the caret; Git: Push to a local bare origin |
| actions | `notification-banner-commands`, `focus-commands`, `desktop-opener` | a toast's button and the editor banner's choices pressed by name; Show Tree, focus between stacked groups, Previous Project, Relaunch Terminal; what reaches the desktop opener (`fakebin/xdg-open`): Open URL as given, file: and javascript: refused, Show in File Explorer's folder |
| interactions | `blame-inline`, `blame-card-follows-its-line`, `selection-mouse`, `terminal-activity`, `find-goto-edges` | the blame card's author and commit, Copy SHA, no blame on an edited line or with the setting off; the card going, pointer still, when focus moves to the other pane of a split and when the caret leaves its line; a held drag autoscrolling the selection to the end, Escape collapsing it, Shift+click; a hidden terminal's output and bell marks; find with no match, wrapping, the match left selected, a seeded query; Go to Line clamping and refusing |
| journeys | `journey-feature-branch`, `journey-rename-refactor`, `journey-layout` | whole sessions across subsystems: branch, new file, build in the terminal, stage, commit past an untracked-file warning, review, switch branches; a rename across files through an unsaved buffer, built, then Discard All; a layout of three groups and a split terminal built up and taken down |
| selection | `line-number-selection`, `word-and-line-drags`, `context-menu-clipboard`, `expand-line-selection`, `find-select-all`, `multi-caret-pairs` | selection where the gesture's granularity is the point: a click on a line number selects the line and a drag over the numbers every line it crosses (up or down), the fold marker still folding; a double-click drag extending by words and a triple-click drag by lines (`App.drag(..., clicks=N)`); a right-click inside a selection keeping it, so the menu's Cut removes the word rather than the line, Paste and Copy by the menu, a right-click outside moving the caret, a whole-line paste keeping the caret's column; Ctrl+L from a caret, a word and an upward two-line selection; the find widget's Alt+Enter selecting every match it counts (match case, a regex), typing over them, one undo; the language's pair rules at three carets (( closing and Backspace deleting each pair, Enter indenting after each brace, Home twice, Ctrl+Backspace, Tab, a quote after a word not pairing) and a pair key surrounding every selection, on a line after a lone quote and inside a string literal (it once replaced the selection) |
| find state | `find-across-tabs`, `find-next-without-widget`, `find-in-split-groups` | the find widget as the editor under it changes: a tab switch by command, Ctrl+PageUp and a tab click each recounting for the buffer now in front and F3 selecting its match (it once kept the previous file's matches and jumped to their coordinates); F3 / Shift+F3 with the widget closed, here and in another file; Replace All typed in one group of a file open in two, the other pane's text, its own widget's count and one undo there; a reload from disk under the open widget recounting |
| caret state | `caret-survives-view-changes` | a caret stays on its character when only the view changes: tab sizes of 8, 2 and 6 by command and by setting (a caret after a tab once moved inside it), a soft-wrap toggle, a bigger editor font, a split; two carets and a selection at once; Down then Up back to the character; the status bar's Col after a tab |
| ux | `nav-history`, `caret-motion`, `indentation`, `find-options`, `git-discard`, `git-discard-all`, `git-commit-body`, `terminal-clipboard`, `notifications`, `tree-keys`, `quick-open`, `undo-semantics`, `mouse-and-wrap`, `search-options`, `groups-keyboard`, `settings-keyboard`, `completion-keys`, `debug-keys`, `terminal-keys`, `recents-and-autosave`, `plugins-ui`, `git-remote`, `compare-keys` | Go Back / Go Forward through files and far jumps (menu state, a closed file reopened, the terminal keeping Alt+Left), Home/End/word/page/document motion, Ctrl+Down scrolling, Ctrl+L and Expand/Shrink Selection, Tab/Shift+Tab over lines, Enter after a brace, Backspace in an indent, whole word/regex by chord and click, Ctrl+H from the find field, a `$1` replace, the query seeded from the word under the caret; untracked files, Stage/Unstage, Discard with Cancel and confirm (disk and open editor), Stage All; Discard All refused (by name) while an affected tab is unsaved, Cancel, then the tracked file restored in its open tab and the untracked and added files trashed with the folder they emptied; a commit body typed over lines with Enter, Backspace and the arrows, committed as subject, blank line and body; editor to shell by Ctrl+Shift+V and the context menu, a terminal row dragged and Ctrl+Shift+C'd into an editor; toasts named, clicked away, stacked, and reached by Focus Notifications; the tree following the active file, type-to-select, F2 rename, Delete with confirm, New Folder, Collapse All; Ctrl+P `file:line`, `>` and `:` prefixes, click to open; undo restoring the replaced selection, one step per multi-caret edit, paste and line move, the caret brought back, redo dropped by a new edit; Shift+wheel sideways, Alt+Z wrap with Down and a double End, a double-click on the tab strip's empty space; Find in Files seeded from the selection, its case/regex toggles, include/exclude globs, Enter on a result; editor groups by keyboard: Ctrl+\\, Ctrl+1/2, the Ctrl+K Ctrl+arrow focus and Ctrl+K Ctrl+Shift+arrow move chords, Ctrl+K Ctrl+0/J, a group closing with its last editor; Settings by keyboard: Space toggles the selected row, Reset, a category jump, Escape, reopening unfiltered; completion by keyboard: Down/Up, Tab to accept, a second Ctrl+Space, the list closing when the caret leaves the word; the debugger by keyboard: F9 on the caret's line, F5 from a plugin's launch configuration and to continue, Ctrl+Shift+F5, Delete in the Breakpoints pane, Shift+F5; the terminal by keyboard: Ctrl+Shift+`, Ctrl+PageUp/PageDown, Shift+PageUp scrollback and the snap back, Alt+Left/Right between split panes, Enter relaunching an exited shell; recent files leading an empty Quick Open, both auto-save modes; the demo plugins' status item (clicked until it updates), decorations, panel surface and colour scheme; Publish Branch, Sync pushing (its label counting what is ahead), Fetch and Pull from another clone through a local bare `origin`, the open editor reloading; a diff by keyboard: opening on its first change, Alt+] / Alt+[, an edit on the working side saved by Ctrl+S, Alt+O to the working file |

The flows live in `flows_*.py`, one module per group; `scenarios.py` holds the
chrome scenarios and the registry. Each module exports `FLOWS` (`{name: f(app)}`)
and may export `PLUGINS` (bundled ones, `plugins/<name>`, or QA-only ones under
`plugins/` here, for test content no bundled plugin ships: `qa-snippets`;
`qa-launch`, a launch configuration for the fixture's binary; `qa-api`, one of
each provider and event) and `ENV`. `scenarios.FLOW_MODULES` lists the modules in
suite order; a `flows_*.py` missing from it, or a scenario name two modules
define, fails the import rather than dropping a flow. The import also rejects
duplicate literal dictionary keys before Python can silently overwrite them,
and plugin/environment metadata naming a scenario its module never defines. Readers several modules
need (`git`, `titles`, `tree_rows`, `search_rows`, `search_hit_names`,
`terminal_tabs`, `messages`, ...) live in
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

## The command-coverage ledger

At the end of every scenario the suite reads the `action-counts` query (each
command that ran since startup, by name, with its count, whatever drove it: a
key, a menu row, a click, the palette or a control command) beside the
`commands` catalogue, and writes their union to `<out>/command-coverage.txt`:
first the registered commands NO scenario dispatched, then those only the smoke
pass ran (`commands-run` runs each once and checks no effect: the ledger counts
them as a gap too), then each command with the scenarios that drove it. The
summary line gives the three counts.
A dry-run sweep (`keys`, `context-menus`, `menu-clicks`) records a receipt and
runs nothing, so it does not count: the ledger measures what was executed. Read
the gap list before writing a new flow, and after it to see the gap close; a
partial run (`suite edit-*`) counts only the scenarios it ran.

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
- The F-keys: xkeyboard-config gives F1-F12 a Ctrl+Alt level that switches
  virtual terminals, and xdotool then sent every F-key wrapped in Alt. Each Xvfb
  starts with `-xkbdir` on a cached copy of the XKB tree whose F-keys are plain
  (`microide_qa.plain_fkey_xkb_dir`); a key the keys sweep says "reached no
  action" in every context is that class again.
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
- **Inside its surface, not overlapping.** A sidebar, debug-pane or panel control
  outside its surface, or two buttons / view tabs / panel tabs that partly cover
  each other with no overlay or modal up, is a finding (a 46-px sidebar drew its
  view tabs past its edge and under the ⋯ button, so a hover lit one control while
  the damage named another).
- **Sanitizer and allocator reports** (`ERROR: AddressSanitizer`, `runtime error:`,
  `WARNING: ThreadSanitizer`, glibc's `malloc():` / `double free`), assertions, and
  the process exiting.
- **What the chrome says, against the buffer it describes.** The redraw verifier
  compares a partial frame with a full render of the SAME state, so a label
  derived from stale state passes it (the find widget kept the previous tab's
  count across a tab switch, every frame correct). After every action the status
  bar's `Ln, Col` must name the focused editor's primary caret (Col visible: a tab
  counts as its width), the find widget's count of a plain query must be its
  occurrences in the buffer, the focused group's active tab must carry the dirty
  mark exactly when its buffer is unsaved and be named after its file, every
  group's visible tabs must be that group's tabs (title and dirty mark, from the
  `editor` query) with its active one highlighted, each terminal must have a panel
  tab naming it (its shell or the title its program set) with the active one
  highlighted, the indent and language segments must name the buffer's own, and the
  window title (`● file — project — microide`) must start with the focused group's
  active tab, dirty-marked exactly when it is -- on X11 `xdotool getwindowname` must
  read the same string, so the taskbar's name is checked too. A disagreement is read again over
  the next frames before it is reported (the labels are derived at frame prep).
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
                               fresh fixture, file churn included (`churn` rows)
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
| `fakebin/` | stand-in programs a scenario puts first on the app's PATH (`zenity`, for the native dialogs; `xdg-open`, which records what the desktop opener was handed) |
| `monkey.py` | the seeded monkey and the command fuzzer's generators, profiles and churn |
| `invariants.py` | the editor-invariants sweep (formerly `tools/sweep-editor-invariants.py`) |
| `run.py` | the CLI |
| `remote.py`, `agents.py` | the remote-project scenarios against a real ssh host (on demand) |
| `local-sshd.sh`, `slow-link.py` | a loopback sshd as the current user for `remote.py`, and its `qa-slow` alias: the same host behind a 60 ms each-way link (`MQA_SLOW_LINK_DELAY_MS`) |
| `weston.sh` | unpacks weston for the Wayland backend |
