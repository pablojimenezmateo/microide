## Why

Once host terminals survive a quit (`remote-terminal-restore`), a user will have
things running on a host they cannot see from here: a `watch` left in a terminal
closed weeks ago, a `clangd` kept for its grace period, a server that was started
by hand and never idles out. The design's answer to "did I forget anything?" is
`microide-server status` and `Remote: Show Status`, and today neither answers it
(verified 2026-10-27):

- `server/status` — what `microide-server status` prints — reports per-workspace
  `terminals` and `processes` as hard-coded zeros (`RemoteServer::StatusJson`,
  "per-workspace accounting: TD"), totals only, no per-terminal or per-process
  rows, and nothing about CPU or memory. `ProcessTable::Process` does not keep the
  argv it was spawned with, and `TerminalTable::Terminal` does not know its
  workspace root, so neither *could* be listed usefully.
- `Remote: Show Status` is a **toast**: `WorkspaceGlobalActionExecutor` passes
  `RemoteHostService::StatusText()` to `Notify`. It states the client's view
  (connection state, sync counts, mirror path, forwards) and nothing the host knows.
- Nothing samples CPU or memory anywhere in the tree; the only `/proc` readers are
  the port sampler and a cwd lookup.

Users ask this question with `htop` over ssh. That needs another terminal, shows
every process on the box, and tells nothing about which microide workspace or tab
a process belongs to. The product should answer it itself, from the client, in one
place, with the rows meaning something in microide's terms (this terminal tab, this
project's language server), and with an action next to each row.

## What Changes

- **The server accounts for what it runs.** `server/status` gains, additively:
  the server's own uptime, RSS and CPU share; per workspace, the open terminals and
  live processes (with the counts no longer zero), the manifest's file count, the
  hashed count and how it watches (native/polling); per terminal, handle, root,
  cwd, title, shell pid, attached clients, exited flag, scrollback lines, and the
  CPU share and RSS of the shell **and everything under it**; per process, handle,
  root, argv (recorded at spawn, bounded), pid, cwd, kept flag and detach time,
  buffered output bytes, CPU and RSS likewise. CPU is a delta between two
  `/proc/<pid>/stat` samples taken on two requests, so an idle server samples
  nothing; one `/proc` walk per request attributes descendants.
  `microide-server status` prints a readable table by default and the JSON with
  `--json`. `kProtocolVersion` stays 3.
- **A Host view in the sidebar.** A contextual sidebar view `host` ("Host"), on the
  same item surface as Outline and Changed on Host, grouped as Server / Workspaces /
  Terminals / Processes with one row each (label and a detail such as
  `3.2% · 41 MB · 2 clients`). It refreshes every two seconds **while visible**
  and never otherwise. Enter on a terminal row focuses its tab, or attaches it in a
  new tab when no tab has it; Delete ends a terminal or process (confirmed when it
  is running); the Server group's row offers Stop Host Server. One view per
  connected host, picked the way the other `Remote:` commands pick a host (the sole
  host, else a prompt).
- **`Remote: Show Status` opens the view** instead of a toast; the status bar's
  remote segment opens it on click; the headless `--control` path keeps
  `StatusText()` as its text answer, now including the host's numbers.
- **Unclaimed terminals and kept processes are the view's first use.** The
  `remote-terminal-restore` row offers Attach/Close once at connect; this view is
  where they are found afterwards.

Out of scope: anything about processes microide did not start (the view is not
`htop`); historical graphs; per-process I/O; memory of the mirror on the client
side (the existing `Remote: Show Status` text already names the mirror); macOS
hosts (the server is a static Linux build, and `/proc` is the only source used).

## Capabilities

### New Capabilities

- `remote-host-activity`: the server's account of what it runs, with CPU and
  memory, and the client's Host view that shows it and acts on it.

### Modified Capabilities

- `remote-server-daemon`: `status` prints per-workspace terminals and processes
  with their CPU and memory, as a table by default; `server/status` carries the
  same, additively.

## Impact

- `src/server/ProcessTable.*`: argv (bounded) and workspace root kept on the
  `Process`; a `List` under the table mutex.
- `src/server/TerminalTable.*`: workspace root kept on the `Terminal` (shared with
  `remote-terminal-restore` § 6.1); `List` returns pid, scrollback lines and the
  rest.
- New `src/server/ProcessActivity.*`: one `/proc` walk per request (pid → parent,
  utime+stime, RSS) and the delta-based CPU share per subtree, with the previous
  sample kept per pid; cleared for pids that are gone.
- `src/server/RemoteServer.cpp`: `StatusJson` filled in; `src/server/ServerMain.cpp`:
  the table printer and `--json`.
- `src/project/remote/RemoteProtocol.h`: the decoded `ServerActivity` record with
  validating decoder (every field untrusted, § 6.9); `RemoteServerClient`: a
  `server/status` call.
- `src/workspace/registries/WorkspaceSidebarRegistry.cpp`, `SidebarMode::Host`,
  `WorkspaceSidebarCoordinator*`: the view, its rows, Enter and Delete.
- `src/workspace/services/RemoteHostService.*`: the sampler (a `work_` job every
  two seconds while the view is visible, mailbox back to the UI), `HostRows(host)`,
  `EndTerminal`, `EndProcess`; `StatusText()` extended.
- Actions: `RemoteShowStatus` repointed; new `RemoteEndHostTerminal`,
  `RemoteEndHostProcess` (`host`, `handle` args); the status-bar remote segment's
  click.
- Tests: kernel (`ProcessActivity` against a spawned child tree; `server/status`
  rows for a terminal and a kept process across two roots; `status` table output),
  fake-ssh (the view's rows for a project with a terminal and a running tool; End
  on each; refresh stops when hidden), `qa-remote` scenario.
- Docs: `remote-projects.md` § 6.12 (`status`) and § 7.6/§ 7.9 (the view),
  `active-work.md`.
