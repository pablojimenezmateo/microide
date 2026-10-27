## Why

Once host terminals survive a quit (`remote-terminal-restore`), a user will have
things running on a host they cannot see from here: a `watch` left in a terminal
closed weeks ago, a `clangd` kept for its grace period, a server that was started
by hand and never idles out. The design's answer to "did I forget anything?" is
`microide-server status` and `Remote: Show Status`, and today neither answers it
(verified 2026-10-27; written against the tree after `remove-host-terminal-command`,
`remote-terminal-restore` and `remote-menu`):

- `server/status` — what `microide-server status` prints — reports per-workspace
  `terminals` and `processes` as hard-coded zeros (`RemoteServer::StatusJson`,
  "per-workspace accounting: TD"), totals only, no per-process rows, and nothing
  about CPU or memory. `remote-terminal-restore` adds the terminal rows (handle,
  root, cwd, clients, exited) and the per-workspace terminal count; the process
  side stays zero. `ProcessTable::Process` does not keep the argv it was spawned
  with, so a process *could not* be listed usefully.
- `Remote: Show Status` is a **toast**: `WorkspaceGlobalActionExecutor` passes
  `RemoteHostService::StatusText()` to `Notify`. It states the client's view
  (connection state, sync counts, mirror path, forwards, and after the restore
  change the unclaimed terminals) and nothing about what the host is spending.
- Nothing samples CPU or memory anywhere in the tree; the only `/proc` readers are
  the port sampler (`/proc/net/tcp`) and a cwd lookup.

Users ask this question with `htop` over ssh. That needs another terminal, shows
every process on the box, and tells nothing about which microide workspace or tab
a process belongs to. The product should answer it itself, from the client, in one
place, with the rows meaning something in microide's terms (this terminal tab, this
project's language server), and with an action next to each row.

## What Changes

- **The server accounts for what it runs.** `server/status` gains, additively,
  the process rows beside the restore change's terminal rows: per process its
  handle, root, argv (recorded at spawn, bounded), pid, cwd, kept flag and detach
  time, exited flag and buffered output bytes; per terminal its shell pid and
  scrollback line count; per workspace the live-process count (no longer zero),
  the manifest's file count, the hashed count and how it watches
  (native/polling). With `{"activity": true}` the same reply also carries the
  resource figures — the server's own uptime, RSS and CPU share, and per terminal
  and per process the CPU share and RSS of the pid **and everything under it** —
  from one `/proc` walk, with CPU as a delta against the previous sample.
  `microide-server status` prints a readable table by default (taking the two
  samples a CPU figure needs) and the JSON with `--json`. `kProtocolVersion`
  stays 3.
- **The figures are pushed while someone watches, and never otherwise.**
  `activity/subscribe` answers with the full status-with-activity and starts the
  server's existing 2-second `PeriodicSampler` pattern (the one `ports/subscribe`
  uses) for as long as any connection is subscribed; each tick pushes
  `activity/changed` to the subscribers; `activity/unsubscribe`, or the
  connection closing, stops it with the last subscriber. No client-side timer
  exists in the app and none is added.
- **A Host view in the sidebar.** A contextual sidebar view `host` ("Host"), on the
  same item surface as Outline and Changed on Host, grouped as Server / Workspaces /
  Terminals / Processes with one row each (label and a detail such as
  `3.2% · 41 MB · 2 clients`). Showing it subscribes; hiding it unsubscribes.
  Enter on a terminal row focuses its tab, or attaches it in a new tab when no tab
  has it (the restore change's `RemoteAttachHostTerminals`); Delete ends a
  terminal (the restore change's `RemoteCloseHostTerminals`) or a process (new
  `RemoteEndHostProcess`), confirmed when it is running; the Server row's Enter
  runs Stop Host Server. One view per connected host, picked the way the other
  `Remote:` commands pick a host (the sole host, else a prompt).
- **`Remote: Show Status` opens the view** instead of a toast (its Remote-menu
  item is the second entry, under Open Folder on Host…). The status bar's remote
  segment keeps opening the Remote menu (`remote-menu`); it does not gain a
  second meaning. The headless `--control` path keeps `StatusText()` as its text
  answer, now including the host's totals from the last reply seen.
- **Unclaimed terminals and kept processes are the view's first use.** The
  `remote-terminal-restore` row offers Attach/Close once at connect; this view is
  where they are found afterwards.

Out of scope: anything about processes microide did not start (the view is not
`htop`); historical graphs; per-process I/O; the client's own cost per host (the
mirror's object store size belongs to the project, and `Remote: Show Status`'s
text already names the mirror); macOS hosts (the server is a static Linux build,
and `/proc` is the only source used).

## Capabilities

### New Capabilities

- `remote-host-activity`: the server's account of what it runs, with CPU and
  memory pushed while subscribed, and the client's Host view that shows it and
  acts on it.

### Modified Capabilities

- `remote-server-daemon`: `status` prints per-workspace terminals and processes
  with their CPU and memory, as a table by default; `server/status` carries the
  same, additively.

## Impact

- `src/server/ProcessTable.*`: bounded `argv` and the workspace root kept on the
  `Process`; a `List` under the table mutex.
- `src/server/TerminalTable.*`: `List` (from the restore change) also returns the
  shell pid and the scrollback line count.
- New `src/server/ProcessActivity.*`: one `/proc` walk per sample (pid → parent,
  utime+stime, RSS), subtree sums per requested pid, the delta-based CPU share
  with the previous sample kept per pid and dropped for pids that are gone; one
  instance shared by the sampler and the one-shot `activity: true` requests.
- `src/server/RemoteServer.{h,cpp}`: `StatusJson(bool activity)` filled in;
  `activity/subscribe`, `activity/unsubscribe`, `activity/changed` and the
  `PeriodicSampler` beside the port one; `src/server/ServerMain.cpp`: the table
  printer and `--json`.
- `src/project/remote/RemoteProtocol.{h,cpp}`: the restore change's
  `ServerStatus`/`ServerStatusFromJson` extended with the process rows and the
  figures (every field untrusted, § 6.9); `RemoteServerClient`:
  `SubscribeActivity(handler)` / `UnsubscribeActivity()` mirroring
  `SubscribePorts`.
- `src/workspace/registries/WorkspaceSidebarRegistry.cpp`, `SidebarMode::Host`,
  `WorkspaceSidebarCoordinator*`: the view, its rows, Enter, and a Delete key case
  on the item surface for modes that declare a row-end action.
- `src/workspace/services/RemoteHostService.*`: `ShowHost(host)` (subscribe on
  show, unsubscribe on hide/mode switch/connection leaving `Ready`, the
  `activity/changed` handler posting to the UI mailbox as `OnHostPorts` does),
  `HostRows(host)`, `EndProcess`; `StatusText()` extended with the totals.
- Actions: `RemoteShowStatus` repointed to the view; new `RemoteEndHostProcess`
  (`remote-end-process <host> <handle>`); the restore change's
  `RemoteAttachHostTerminals` / `RemoteCloseHostTerminals` reused for the
  terminal rows.
- Tests: kernel (`ProcessActivity` against a spawned child tree; `server/status`
  with `activity` for a terminal and a kept process across two roots; the
  subscription pushes every tick while subscribed and nothing after the last
  unsubscribe; `status` table output), fake-ssh (the view's rows for a project
  with a terminal and a running tool; End on each; no `activity/changed` after
  the view hides), `qa-remote` scenario `V`.
- Docs: `remote-projects.md` § 6.12 (`status`) and § 7.6/§ 7.9 (the view),
  `active-work.md`.
- Sequencing: after `remove-host-terminal-command`, `remote-terminal-restore` (the
  terminal rows, the `ServerStatus` decoder, the attach/close ids) and
  `remote-menu` (the Show Status item and the segment's click).
