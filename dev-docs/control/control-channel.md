# Control channel

The control channel lets an external tool (typically an LLM) drive a running
microide instance, and lets a fresh launch open straight into a ready-to-debug
state. It is the headless counterpart to the command palette: everything routes
through the same command chokepoint, so the channel adds transport + a small
vocabulary, not a parallel control path.

## Entry points

- **Headless `--control`** — `microide --control` force-starts the channel
  (bypassing the `control.enabled` gate) *and* mirrors every response, event, and
  cold-start `applied` line to **stdout as JSONL** (`WorkspaceShell::
  ForceStartControlChannel` → `ControlChannelService::SetStdoutMirror`). This is
  the entry point for an LLM/agent: it can drive the instance and observe results
  from one stdout stream, with or without ever opening the socket. The real window
  stays running and fully interactive. Because `--control` owns the channel
  lifecycle for the whole run, a live settings change (e.g. a spec's `set-setting
  debug.enabled true`) never tears the socket down — `MaybeStartControlChannel`
  early-returns when `control_stdout` is set, so the advertised socket persists.
- **Live channel** — a per-instance AF_UNIX socket. Gated on the
  `control.enabled` setting (off by default); toggling it on starts the listener
  immediately (no restart), because the SDL wake event is always registered and
  only the listener is gated (`WorkspaceShell::MaybeStartControlChannel`). It can
  also be started transiently with `microide --set control.enabled true` (the
  override is never persisted — see below).
- **Cold-start spec** — `microide --control-spec <file.json>` opens a project
  with breakpoints already set (and optionally settings applied, files revealed, a
  session started) before the window is interactive. The spec's `project` field
  selects the project (and wins over a restored session); the spec is translated
  to command lines and applied via `WorkspaceShell::ApplyControlSpec`, which emits
  one `{"applied":...}` JSONL line per entry.

## Deterministic settings (`set-setting`, `--set`, spec `settings`)

- `set-setting <id> <value>` is a normal command (palette / socket / spec): it
  routes through the `SetSettingValue` chokepoint and **persists** like the
  Settings overlay. Unknown id / invalid value rejects with `ok:false`.
  **Use `--set` instead for measurement work.** A persisted setting outlives the
  run that set it and quietly changes every launch after it — an A/B that left
  `project.files_exclude` set cost the next plain launch profile a whole extra
  tree walk, which is how TD-2026-08-17-255 was found, but only after the numbers
  had been confusing for a while.
- `--set <id> <value>` (repeatable) and the spec `settings` key apply the same
  values **transiently**: live this session but never written to the saved config.
  They are tracked in `WorkspaceContext::transient_setting_keys` and stripped by
  the persistence coordinator before `SaveUserConfig`/`SaveConfigState` serialize,
  so a headless drive can flip `control.enabled`/`debug.enabled` without clobbering
  the user's config. The debugger also auto-enables (transiently) when a spec
  carries breakpoints or a launch, removing the old ordering trap.

## Headless agent runbook

```bash
# Drive an instance and stream results as JSONL on stdout:
microide --control --control-spec /tmp/debug.spec.json
```

Spec (`/tmp/debug.spec.json`):

```json
{
  "project": "/path/to/project",
  "breakpoints": [{"file": "src/main.cpp", "line": 42}],
  "open": ["src/main.cpp"],
  "launch": "My Launch Config"
}
```

The stdout JSONL stream, in order:

1. `{"event":"ready","pid":..,"socket":"..","project_root":".."}` — handshake.
2. one `{"applied":"<command>","ok":true|false,"error":".."}` per spec entry
   (settings, auto-enable, breakpoints, opens, launch, raw commands).
3. `{"event":"output",...}` / `{"event":"stopped",...}` /
   `{"event":"terminated",...}` as the session runs. Each stop emits the `stopped`
   event **twice** (see below).

### Driving a live instance headlessly

To exercise a long-lived `--control` instance (rather than a one-shot spec):

```bash
xvfb-run -a env SDL_AUDIODRIVER=dummy XDG_RUNTIME_DIR=/run/user/1000 \
  ./build/microide/microide <project> --control &
XDG_RUNTIME_DIR=/run/user/1000 ./build/microide/microide control-send <verb> [args]
```

Three gotchas, each of which cost real time:

- **It must run under `xvfb`.** With `SDL_VIDEODRIVER=dummy` alone (no X display)
  the app exits right after the sandbox-init log line. xvfb keeps the window alive.
  (This is the opposite of the perf harness, which wants the dummy driver — see
  `dev-docs/performance/perf-harness.md`.)
- **`XDG_RUNTIME_DIR` must be a SHORT path.** The control socket is
  `$XDG_RUNTIME_DIR/microide/<pid>.sock`, and AF_UNIX caps the path at 108 bytes.
  A long scratch path silently fails to bind — no socket, no error. Use the real
  `/run/user/<uid>`.
- **Kill by PID, never `pkill -f 'build/microide/microide'`** — that pattern
  matches the invoking shell's own command line. Use
  `ps -eo pid,args | awk '$2 ~ /\/microide\/microide$/ {print $1}'`.

Read an edit back with the `buffer` query (the text, carets, selection and folds of
the buffer the caret is in) or by driving `save` and reading the file from disk.
Watch for cross-command state: `add-cursor-all-matches`
leaves multiple carets that pollute later single-caret edits, and a buffer right
after `open` may not have settled — settle before asserting.

Discover names without reading plugin source with the one-shot client:
`microide control-send --query launch-configs` or `microide control-send --query
adapters` (see below).

## Architecture

```
client ──JSONL──▶ ControlSocketServer (platform, 1 I/O thread)
                     │ inbound queue + SDL wake          ▲ outbound / broadcast
                     ▼                                   │
                  ControlChannelService (main thread) ───┘
                     ├─ command → execute_command_line → CommandPromptCoordinator
                     ├─ query   → read WorkspaceContext → JSON
                     └─ debug hooks → broadcast stopped/terminated/output events
```

- `src/platform/ControlSocketServer.{h,cpp}` — single-threaded poll loop over the
  listen fd, a wake pipe, and every client fd. Inbound lines are queued for the
  main thread; replies/broadcasts are queued from the main thread and flushed by
  the I/O thread. Mirrors the DAP client's marshaling discipline: the background
  thread never touches host state.
- `src/workspace/control/ControlChannelService.{h,cpp}` — owns the server, drains inbound
  requests on the control SDL wake event (`ConsumeControlCallbacks`), dispatches
  commands through the `Operations.execute_command_line` seam, answers queries by
  reading `WorkspaceContext`, and broadcasts debug events (wired from the
  `DebugService` operations in `WorkspaceShellPlugins.cpp`).
- `src/workspace/control/ControlProtocol.{h,cpp}` — request/response/event (de)serialize
  plus `ControlChannelHelpText()`, the single source of truth for the protocol
  text rendered by `microide control-help` and the man page.
- `src/workspace/control/ControlSpec.{h,cpp}` — parse the cold-start spec (incl. the
  `settings` key) and translate it to command lines (`QuoteCommandArg` keeps
  arguments `ParseCommandLine`-safe).
- `src/workspace/shell/WorkspaceShellLsp.cpp` — thin shell forwarders, plus
  `ApplyControlSpec` (applies transient settings + auto-enable + emits `applied`
  lines), `ForceStartControlChannel`, and `ApplyStartupSettingOverrides`. The
  stdout sink (`emit_jsonl`) and `adapter_types` are wired in
  `WorkspaceShellPlugins.cpp`.

## Protocol

Newline-delimited JSON, one object per line.

| Direction | Shape |
|-----------|-------|
| request (command) | `{"id":1,"command":"breakpoint-set src/main.cpp 42"}` |
| request (query)   | `{"id":2,"query":"debug-state"}` |
| response (command)| `{"id":1,"ok":true,"feedback":"..."}` / `{"id":1,"ok":false,"error":"..."}` |
| response (query)  | `{"id":2,"ok":true,"result":{...}}` |
| event (stop began)| `{"event":"stopped","reason":"breakpoint","threadId":1,"framesPending":true}` |
| event (stop resolved)| `{"event":"stopped","file":"x.py","line":42,"reason":"breakpoint","threadId":1,"frames":[...],"framesPending":false}` |
| event (terminated)| `{"event":"terminated","sessionId":1}` (clean) / `{"event":"terminated","sessionId":1,"reason":"debug adapter exited unexpectedly"}` (crash/kill/launch-reject) |
| ready (stdout)    | `{"event":"ready","pid":..,"socket":"..","project_root":".."}` |
| applied (stdout)  | `{"applied":"breakpoint-set ..","ok":true}` |

Query verbs: `debug-state`, `breakpoints`, `function-breakpoints`,
`exception-filters`, `tabs`, `projects`, `status`, `launch-configs`, `adapters`,
`editor`, `commands`, `terminals`, `terminal-output`, `notifications`,
`keybindings`, `settings`, `ui`, `buffer`, `output`, `action-counts`, `hit-test`.

**Where a press lands (`hit-test`).** Args `{x, y}` in layout units; answers
`{region}`: `normal` (the app gets the press), `draggable` or `resize` (the title
bar's drag area and the window's resize edges: SDL hands the press to the window
manager and the app never sees it), or `outside` the window as it is now sized.
`tools/qa` asks before every press it sends through X, so it waits for the press's
button events only when the app will get them.

**Debug state.** `debug-state` reports the active session's lifecycle as
`session` (`none`, `initializing`, `configuring`, `running`, `stopped`,
`terminated`, `failed`) beside the stop it shows (`stopped`, `reason`,
`threadId`, `frames`, and `file`/`line` of the focused frame): a driver waits for
`running` before it pauses, which `stopped: false` alone cannot tell from no
session at all. `sessions` lists every live session as `{id, label, state,
active}`, in creation order.

**Settings.** `settings` lists every registered setting, built-in and
plugin-contributed, with its current value: `{id, label, description, type
(bool|int|float|string|enum), scope (user|project), plugin?, group, default,
value, enumValues?, min?, max?, step?, suggestsFonts?}`. `value` is the resolved
value as `set-setting` would write it; a driver sweeping the catalogue takes what
to set from `type` (both booleans, every `enumValues` entry, the ends of a
numeric range).

**Keybindings.** `keybindings` lists the resolved bindings in dispatch order:
`{id, chord, context, skipsTerminal, plugin, command}` (`command`: the command the
binding runs, as the `ui` action receipt names it), `chord` spelled as the menus show it
(`Ctrl+Shift+F`), `context` one of `global`, `editor`, `sidebar`, `terminal`. A
`global` binding is active everywhere but a focused terminal, and there too when
`skipsTerminal`.

**What is on screen (`ui`).** The locator surface for automation (`tools/qa`):
`{window:{width,height,scale}, focus, modal, sidebar:{visible,view,temporary},
overlay:{visible,mode}, menu:{open,menu,activeItem,submenu,contextMenu,overflow,rowsRun,lastRun},
panel:{visible,content,maximized}, surfaces:{menuBar,projectTabStrip,tabStrip,
breadcrumb,editorSurface,sidebar,rightPane,bottomPanel,statusBar}, elements:[...]}`.
`modal` names the surface that owns the keyboard and pointer (`settings`, `help`,
`prompt`, `unsaved-prompt`) or is null. Each element is `{name, kind, rect:[x,y,w,h], label?, enabled?, active?,
selected?}` in layout units -- the coordinates the app's mouse paths test, which are window coordinates only at `ui.scale` 1 on a 1x display. `window.scale` is `[x, y]` window coordinates per layout unit (ui.scale and the display's pixel density): a driver multiplies a rect, or any geometry field below, by it before pointing (tools/qa's App does it in one place). Names, by kind:

| name | element |
|------|---------|
| `menu:<label>` | a menu-bar item (`menu:Edit`) |
| `menu-item:<menu>/<label>` | a row of the open menu, submenu (`menu-item:Edit/Undo`), tree context menu (`menu-item:context/...`) or overflow popup (`menu-item:more/...`); separators are not elements |
| `project-tab:<title>` | a project tab |
| `tab:<group>/<title>` | an editor tab of split group `<group>` (0-based) |
| `sidebar-view:<view id>` | a sidebar view button (`tree`, `search`, `git`) |
| `row:<view id>/<label>` | a visible sidebar row (`row:tree/src`; `#<index>` when a row has no label) |
| `panel-tab:<title>` | a bottom-panel tab |
| `status:<segment>` | a status-bar segment (`project`, `branch`, `language`, `indent`, `encoding`, `line-column`, `problems`, `lsp`, `layout-mode`, `remote`), `enabled` when a click does something, and `menu` naming the menu a click opens in place of a command (`remote` opens `Remote`, anchored at the segment); `status:plugin/<plugin>/<item>` is a plugin status item (drawn in the breadcrumb row), its text as `label`, `enabled` when a click runs a command |
| `button:<surface>/<label>` | `window/{minimize,maximize,close}`, `close-tab/<group>/<title>`, `sidebar/{more,Collapse,Refresh}`, `panel/{new-terminal,maximize}`, `banner/<label>`, `banner/dismiss`, `notification/<label>`; in a merge tab `merge/{prev,next,open,save,Unresolved,Toggle Base,Mark Resolved}` and, per visible conflict `i`, `merge/<i>/accept-incoming`, `merge/<i>/accept-current`, `merge/<i>/result-{base,incoming,current,both}` (hover one to show it, as a person does) |
| `divider:<which>` | `sidebar`, `panel`, `right-pane` resize handles; `editor/<n>` split dividers |
| `scrollbar:<surface>` | a scrollbar on screen, its track as `rect` plus its `thumb` rect and `vertical`/`horizontal` as `label`: `editor/<group>` (and `editor/<group>/h`), `sidebar`, `terminal/<pane>`, `panel` (Output, a plugin surface), `overlay`, `settings`, `settings/categories`, `compare` / `merge` (and `/h`), `debug` |
| `overlay-row:<label>` | a visible row of the open list overlay (file finder, command palette, project search, pickers, completion, code actions), by its painted primary text; `selected` marks the one Enter runs |
| `field:<surface>/<name>` | a text field, its text as `label`: `find/search`, `find/replace` (the in-file find widget), `prompt` (a prompt's input), `settings/filter`, `search/{query,replace,include,exclude}` (the search view), `git/subject`, `git/body` (the commit panel); `active` marks the one being edited |
| `button:find/...` | the find widget's `previous`, `next`, `close`, `replace`, `replace-all`, `match-case`, `whole-word`, `regex` (`active` = toggle on) |
| `button:prompt/<label>` | a prompt's buttons (`Save`, `Don't Save`, `Cancel`; a confirm's two) |
| `button:search/...`, `button:git/...` | the search view's `mode`, `case`, `hidden`, `scope` toggles; the git view's `stage-all`, `discard-all`, `refresh`, `branch`, `sync`, `commit`, `confirm-commit` (labels as drawn: the branch name, Sync with its behind/ahead counts; `enabled` for Stage All, Discard All and Commit) |
| `setting:<id>` | a Settings row (value as `label`, `on`/`off` for a checkbox), with `button:setting/<id>/{checkbox,decrement,increment,value,reset,scope}` for its controls |
| `settings-category:<label>` | a Settings category in the left rail |
| `button:debug-toolbar/<id>` | the floating debug toolbar's `continue`, `step-over`, `step-into`, `step-out`, `reverse-continue`, `step-back`, `restart`, `stop` (tooltip as `label`; `enabled` false for a step while the program runs) |
| `notification:<index>` | a toast, by its index in the `notifications` query, its message as `label` (`active` = the keyboard is on it); a click dismisses it |
| `debug-tab:<mode>` | the debug pane's mode tabs: `variables`, `breakpoints`, `watch`, `callstack` (`active` = shown) |
| `debug-row:<mode>/<index>` | a visible debug pane row, `<index>` into that mode's rows, with its painted text as `label` (`name = value : type` for variables and watches, `function  file:line` for frames); `selected` marks the focused frame or selected row |

`menu.lastRun` is the receipt for a menu row: the `<menu>/<label>` locator (the
`menu-item:` element's name without its kind) of the row a click or Enter last
ran, and `menu.rowsRun` counts the rows run since startup. A driver that clicks
`menu-item:File/Save` checks that `rowsRun` went up by one and `lastRun` is
`File/Save`: a press taken by a surface beneath the menu shows as no run, or as
another row's. With `MICROIDE_QA_MENU_DRY_RUN=1` at startup a row records its run
and closes the menu without running its action, so a sweep can click every row
(Quit and Close Project included); `tools/qa`'s `menu-clicks` scenario does.

Beyond the elements, `ui` carries state no element does. `windowTitle` is the
title the last prepared frame gave the window (`● file — project — microide`). With an overlay open,
`overlay` adds `query` (its text field), `count` (rows in the list), `kind` for a
quick pick (`launch-config`, `color-theme`, `document-symbol`, `workspace-symbol`, `language-mode`) and `selected`
(the row Enter runs). With a prompt up, `prompt` is `{kind: unsaved|input|confirm,
title, message, detail?, input?}`. `popups` is `{hover: {kind: blame|diagnostic|symbol|
debug-value, text, rect, action?: {label, rect}} | null, signatureHelp: <label> | null, signatureHelpDetail: <active parameter, then docs> | null}`
(a blame card's `text` is its author, date and summary lines; `action` is the
card's button, Copy SHA or Quick Fix). `editors` lists every
editor pane's text geometry -- `{group, focused, rect, textX, firstLineY, lineHeight,
charWidth, gutterX, gutterWidth, foldHitX, stickyRows, scrollColumn, rows:[{line, y}]}`,
`rows` naming the buffer line each visible row shows (a collapsed fold skips lines) --
so a driver clicks a line and column at `textX + (column - 1 - scrollColumn + 0.2) *
charWidth` and a row's `y + lineHeight / 2`, and a fold marker between `foldHitX` and
`gutterX + gutterWidth`, without knowing the font. `terminal`, while a terminal pane shows, is the active
pane's `{rect, textX, firstLineY, lineHeight, charWidth, firstRow, visibleRows}`:
transcript row `r` (the `terminal-output` line) and column `c` sit at `textX + (c + 0.3) *
charWidth`, `firstLineY + (r - firstRow + 0.5) * lineHeight`.

**What the caret is in (`buffer`).** The focused group's active buffer (an editor, a
compare tab's editable side, a merge result): `{available, kind, path, language,
dirty, readOnly, softWrap, tabSize, softTabs, revision, lineCount, scrollLine,
visibleCount, text, cursors, selectedText, folded, foldable}`. `text` joins the lines
with `\n` (omitted with `textTruncated` past 1 MiB). `cursors` is every caret, the
primary first, each `{line, column, anchorLine, anchorColumn, empty}` (1-based byte
columns). `folded` lists the collapsed folds and `foldable` the fold ranges resolved
in the current window, each `[openerLine, closerLine]`. With no buffer it answers
`{available:false}`. `tools/qa` checks every edit against this rather than against
the saved file.

**What an output channel holds (`output`).** `{channels, shown, channel, text}`:
every output channel as `{id, label, lines}`, the one the panel's Output tab shows,
and the lines (the newest 10000) of `channel` -- the args' `{"channel": id}`, else the
shown one. References, call hierarchy (`lsp.callHierarchy`), workspace symbols
(`lsp.workspaceSymbols`), the debug console and the logs all print here, so a
driver reads their results from it.

**What ran (`ui.actions`).** `{dispatched, last, source, rejected}`: the count of
actions dispatched through the action coordinator since startup, the last one's
command name, its source (`shortcut`, `menu`, `context-menu`, `command`) and
whether it was rejected. A `menu-item:` row also carries the `command` it runs.
With `MICROIDE_QA_ACTION_DRY_RUN=1` at startup every key-, menu- and
pointer-driven action records its receipt and runs nothing (control commands still
run), which is how `tools/qa`'s `keys` and `context-menus` sweeps press everything.

**What ran, counted (`action-counts`).** `{dispatched, commands: {<command>: n},
plugins: {<plugin command>: n}}`: every command that ran since startup with how
many times, named as `actions.last` names it (the command name, else the palette
label), and the plugin commands by theirs. `tools/qa`'s suite reads it at the end
of every scenario and unions the names into a command-coverage ledger
(`<out>/command-coverage.txt`): a registered command no scenario drove is listed
there, so the catalogue's gaps are measured, not remembered.

A name shared by two elements gets `#2`, `#3`, ... on the later ones in visual
order, so a name resolves to one element. Every rect comes from the geometry the
matching mouse path hit-tests, so clicking an element's center lands on it.
Answering reads state only: it requests no redraw and needs no painted frame.

**Notification buttons.** `notifications` lists every toast oldest first:
`{index, key, tone, message, sticky, progress?, actions}`, each action
`{index, label, command, args}`. `notification-action <row> <action>` presses one
exactly as a click does: the row by its `key` (else its index), the button by its
label (exact, then case-insensitive; else its index). It runs through the same
action executor, dismisses a transient row afterwards unless the action keeps it
open, and leaves a sticky row to its owner. `focus-notifications` is the keyboard
route (arrows, Enter, Delete, Escape) a person uses for the same buttons.

**Remote projects.** For a remote active project `status` carries
`remote: {host, hostRoot, state, files, filesPending, unsentPushes, conflicts,
heldDeletes}`; `state` is the connection's (`disconnected`, `connecting`,
`needs-auth`, `starting`, `installing`, `reconnecting`, `offline`) or, on a live
link, `syncing` until the first sync lands and `ready` after. `projects` marks a
remote entry with `host` and `hostRoot`. A driver waits on `state == "ready"`
instead of reading the sync row's text.

**Editor banner buttons.** A `save` refused because the file changed on disk
answers with the banner's choices; `banner-action <Compare|Reload|Overwrite|Keep>`
presses one on the active tab's banner exactly as a click does (label
case-insensitive; `Retry` on a failed open). It rejects when the active tab shows
no banner or the banner has no such button. In a remote project this is the
conflict flow of `dev-docs/design/remote-projects.md` § 7.7: the host's version is
what the mirror pulled, Compare shows it beside the unsaved buffer, Overwrite
pushes the buffer to the host, Reload takes the host's.

The last four are the agent-facing ones (ported 2026-10-07 from the never-merged
`feat/editor-groups` control work, and made pane-aware):

- `editor` returns every editor group (`index`, `focused`, `tabs`) and the split
  tree in its persisted pre-order form (`split`: `{orientation, weights}` per node;
  a `leaf` is one group, in group order). Each tab carries `kind`, `path`,
  `title`, `active`, `dirty`, 1-based `cursorLine`/`cursorColumn`/`scrollLine` and
  `foldedLines` (the 1-based opener of each collapsed fold); a file still loading
  off-thread reports `loading: true` and the view it will open with. The focused group's active tab also reports `visibleTop`/`visibleCount`
  — what the user is looking at. A diff tab reports `changes` (its hunk count),
  `ignoreWhitespace`, `showWhitespace`, `leftLabel`/`rightLabel`, in a branch
  review `reviewedHunks`/`notedHunks` (the hunks drawn marked reviewed or with a
  note) and, inside a review of several files, `reviewFiles` and the 1-based
  `reviewFileIndex`.
  `tabs` stays as the flat list the media scripts use.
- `commands` lists the runnable command registry as `{command, usage, label}`, then the plugin-contributed commands (`plugin: true`).
- `terminals` lists the terminal strip: per tab `{tab, active, unseenOutput,
  unseenBell, panes}`, per pane `{pane, active, label, labelPrefix, running, pending, hostHandle, lineCount}` (`labelPrefix`: what the tab strip shows before the label, e.g. `ssh · ` for the authentication tab; `pending`: a remote project's terminal waiting for its connection; `hostHandle`: the host terminal a remote project's pane is, 0 for none).
  `unseenBell`: a program rang the bell (BEL) while the tab was not on screen.
- `terminal-output` snapshots one pane's scrollback: args `{tab, pane, lines}`
  default to the active tab's active pane and the last 1000 lines (capped at
  20000). It returns `{tab, pane, running, lineCount, text}` with trailing blank
  rows trimmed. PTY output is asynchronous, so this reads what is already
  buffered: run a command with `term`, then poll until `running` is false (no exit
  code is exposed).
- Driving an interactive program in a terminal: `type <text>` PASTES (bracketed
  paste when the program enabled it), and `key <chord> [count]` presses keys —
  `key enter`, `key down 3`, `key shift+enter`, `key ctrl+c` — through the
  window's real keyboard path, so the terminal encodes them exactly as a
  keystroke (kitty keyboard protocol included) and focus/shortcut routing
  applies. A printable key types its character as a keyboard does (`key y`
  answers a `[y/N]` prompt; `key shift+n` types `N`); a chord with Ctrl, Alt or
  Super types nothing.
- The mouse: `pointer [mods+]<verb> <x> <y> [button|notches] [clicks]`, at a
  point in layout units -- the coordinates the `ui` query's rects are in, so a
  driver clicks an element at its rect's center with no window-system input
  (a Wayland session, an agent). `pointer click 640 12` presses and releases the
  left button, `pointer click X Y right`, `pointer click X Y left 2` is a double
  click, `pointer ctrl+click X Y` holds Ctrl for it, `pointer down`/`move X Y
  left`/`up` is a drag, `pointer wheel X Y 3` scrolls three notches down
  (negative: up), and `pointer drop X Y <path>` releases a file dragged in from
  another application. Every verb moves the pointer there first, so hover state
  follows it. `term <command>` runs the command through `$SHELL -lc`; an argument
  quoted when typed stays one word (`term bash -c "make; echo done"`). Worked
  example, used to check Claude Code runs in the terminal: `panel-maximize`,
  `term claude`, `key down`, `key enter`, `type hello`, `key shift+enter`,
  `terminal-output`.

Agent navigation commands: `reveal <path> <line[:col]>` opens the file, puts the
caret on the 1-based line and centres it (a large file still loading lands there
when the load completes); `tab-to-group <group> [slot]` moves the focused pane's
active tab into editor group `group` (counting from 1 in layout order, as `focus-group` and Ctrl+1/2 do), at `slot` or the end.
Events: `stopped`, `terminated`, `output`. With `--control` (stdout mirror on),
events surface even with zero socket clients; responses and `applied` lines are
mirrored too. `ready`/`applied` lines are stdout-only.

**Two-phase `stopped`.** Every stop emits the `stopped` event twice, disambiguated
by `framesPending`. The first lands the instant the adapter halts, carrying the
real `reason`/`threadId` with `framesPending:true` (no `file`/`line`/`frames`) — so
an agent learns it stopped within ms even while a slow adapter (e.g. gdb indexing
DWARF for minutes) resolves the call stack. The second lands once the stack
resolves, carrying `file`/`line`/`frames` with `framesPending:false`. Only the
active session broadcasts; a background-session stop does not emit either phase.

**`terminated` fires for every end.** A session broadcasts `terminated` on *any*
terminal transition — a clean DAP `terminated`/`exited`, a launch/attach rejection,
or the adapter process dying with no DAP event (crash / external kill / RLIMIT_AS
cap). A non-clean end carries a `reason`; a clean exit omits it. This is driven off
the `DebugSession::on_terminated` callback (fired once via the absorbing terminal
guard), so an observer is never stranded waiting on a `terminated` that a crashing
adapter never sent. The `sessionId` is the real session id.

Line numbers are 1-based on every developer-facing surface (commands, spec,
events, queries); the single 1-based→0-based conversion lives in the
breakpoint-command executor (`WorkspaceGlobalActionExecutor.cpp`).

## Breakpoint commands

`breakpoint-set`, `breakpoint-remove`, `breakpoint-enable`, `breakpoint-disable`,
`breakpoint-condition`, `breakpoint-hit-condition`, `breakpoint-logmessage`,
`breakpoint-clear`, plus session launch via `debug-launch [name]` (named config)
or `debug-run [--type <adapter>] <program> [args...]` (ad-hoc launch by program
path — no pre-defined config needed). All breakpoint commands take an explicit
`<file> <line>` (unlike the context-menu breakpoint modifiers, which read the
gutter line). Breakpoint/debug commands require `debug.enabled`, but over the
control channel any `breakpoint-`/`debug-` command **auto-enables it transiently**
(no `set-setting debug.enabled true` prelude); they re-send to a live session via
`DebugService::ResendBreakpointsForFile`. Breakpoints placed this way persist
through the normal `PersistedDebugState` path.

**Function breakpoints** break on a symbol with no file/line — the right tool when
an agent is told only a function name: `breakpoint-function-add <name>`,
`breakpoint-function-remove <name>`, `breakpoint-function-toggle <name>`,
`breakpoint-function-condition <name> [expr]`. Exception-filter conditions use
`breakpoint-exception-condition <filterId> [expr]`, refused (with the filters
known) for an id no adapter advertised and for a condition on a filter whose
`supportsCondition` is false. Query current state with the
`function-breakpoints` and `exception-filters` verbs.

## Review / diff commands

Three verbs let an agent (or a human via the control channel) bulk-open the
diff/merge tabs needed to review changes. Each one switches to the **Source
Control** view, dedupes against already-open tabs, and closes stale *clean* review
tabs from a prior run (dirty/edited review and merge tabs are always kept). The
reply `feedback` summarizes counts and lists the opened files, e.g.
`review-conflicts: opened 3, reused 1, closed 1 — src/a.cpp, src/b.cpp, src/c.cpp`.

- `review-conflicts` — one **merge** tab per conflicted file in the working tree.
  **Non-mutating**: it never runs `git merge`. Run the merge yourself first, then
  open the conflicts:
  ```bash
  microide control-send term "git merge origin/main"
  microide control-send review-conflicts
  ```
- `review-branch [ref]` — one **compare** tab (working tree vs `ref`) per differing
  file, i.e. "what is different between local state and `ref`". `ref` is any
  commit-ish (branch, tag, hash, `HEAD~3`). Omit it (or pass `origin`) to use the
  repo base branch via `ResolveGitBaseReference` (origin/HEAD, else main/master,
  else `@{upstream}`).
- `review-commit [commit]` — one **compare** tab (`commit~1` vs `commit`) per file
  that commit changed; `commit` is any commit-ish and defaults to `HEAD` (review the
  last commit). Pass a hash to review any historical commit.

The orchestration lives in `ReviewSessionCoordinator` (host-owned tab lifecycle +
Source Control switch injected as callbacks); the pure open/reuse/close
reconciliation is `ComputeReviewTabPlan` (`src/workspace/git/ReviewTabPlan.h`). File
enumeration reuses `project::CollectGitWorkingTreeEntries` (conflicts),
`CollectGitWorkingTreeDiffFiles` (branch), and `CollectGitCommitChangedFiles`
(commit); tab opening reuses `CompareMergeService` (which dedupes internally).

## One-shot client (`microide control-send`)

`microide control-send` is the reliable way to drive a running instance from a
script or an agent — no socket plumbing, no JSON hand-crafting. It connects to the
target instance (auto-selecting the sole running one, else `--pid`/`--socket`),
sends one request, keeps the connection open until the reply (and, with `--wait`,
the awaited event) arrives, prints every JSONL line to stdout, and exits:

```bash
microide control-send breakpoint-function-add main      # {"command":"..."}
microide control-send --query launch-configs            # {"query":"..."}
microide control-send --json '{"command":"debug-start"}' # sent verbatim
microide control-send debug-run ./build/app --wait stopped  # block until it stops
```

Options: `--pid <n>` / `--socket <path>` (target), `--timeout <secs>` (default 5),
`--wait <event>` (block until that event; `stopped` waits for the resolved frame),
`--id <n>`. Exit codes: `0` reply ok, `1` reply not ok, `2` usage/discovery/
connect, `3` `--wait` timed out. The reliability rests on the server's graceful
half-close: a client that closes its write side still receives replies for the
requests it already sent (`ControlSocketServer` lingers until they flush), so the
old half-closed-client "no reply" footgun is gone — though `control-send` is still
the recommended primitive.

## Recipes

**Set up a session and hand the live window to the human** (the "give me control"
request). `--set control.enabled true` *without* `--control` opens a normal
interactive window with the socket live — the human keeps driving after the agent
finishes. Any `breakpoint-`/`debug-` command auto-enables the debugger, so no
`debug.enabled` prelude is needed:

```bash
microide /path/to/project --set control.enabled true &   # normal window + socket
microide control-send breakpoint-function-add A          # debugger auto-enables
microide control-send debug-run ./build/app              # or: debug-launch <config>
# leave the window open — the human now drives it interactively.
```

`debug-run [--type <adapter>] <program> [args...]` synthesizes a transient launch
config from a binary path, so no pre-defined launch config is required (the
bundled `gdb-dap` plugin supplies a `gdb` adapter for native code). Define a
project launch config of type `gdb` for a reusable target.

**Investigate a crash, break just before the suspect line** (headless): use
`--control --control-spec` with a `{file,line}` (or `functionBreakpoints`) entry,
read launch names via the `launch-configs` query first, then watch for `stopped`.

## Discovery & security

Socket: `$XDG_RUNTIME_DIR/microide/<pid>.sock` (0600). Descriptor:
`$XDG_RUNTIME_DIR/microide/instances/<pid>.json` with `{pid, socket,
project_root, project_hash}`. `microide control-list` enumerates them. The
channel grants command-palette-level power, so it is opt-in per instance and the
socket is user-private.
