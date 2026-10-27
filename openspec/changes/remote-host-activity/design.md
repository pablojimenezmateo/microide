## Context

The server already answers `server/status` (what `microide-server status` prints)
and the client already has `Remote: Show Status`, `Remote: Show Log` (a host job
through `RemoteHostService::work_`, shown in an Output channel) and `Remote: Stop
Host Server`. The sidebar already has a contextual, host-owned view on the plugin
item surface — Changed on Host (`SidebarMode::HostChanges`, rows built by
`RemoteHostService::HostChangeRows`, Enter routed by
`WorkspaceSidebarCoordinatorActions`). What is missing is the data (the server
keeps no argv, no root per terminal, no resource figures) and a surface that is a
view rather than a toast.

Priorities, in the repo's order: speed (the view costs nothing until it is opened,
and one request per refresh after), correctness (every figure is attributed to a
microide object — a tab, a project's tool — or not shown), CPU/memory (no sampling
thread on the server, no walk of `/proc` except on request, no per-frame work on
the client), and reuse (one item surface, one host-job pattern, one status method).

## Goals / Non-Goals

**Goals:**
- From the client, see what a host is running for microide: server, workspaces,
  terminals, processes, with CPU and memory, in microide's own terms.
- Act on it: focus or attach a terminal, end a terminal or process, stop the server.
- The same account from the host's shell (`microide-server status`).
- Zero cost while nobody looks.

**Non-Goals:**
- A general process viewer; anything microide did not start is not listed.
- History, graphs, per-process I/O or network figures.
- Sampling on a timer inside the server.
- macOS hosts (the server is Linux; the client side is portable and shows what the
  server sends).

## Decisions

### D1. `server/status` grows; no new method
The method exists, the CLI prints it, and every field is additive, so the wire
version stays 3 and an older server simply lacks the new keys, which the client's
decoder treats as "not reported". A dedicated `server/activity` would duplicate
the top half of `status` for no gain.

Shape (new keys only):

```
server:     {uptime_ms, rss_bytes, cpu_permille, idle_since_ms}
workspaces: [{root, clients, terminals, processes, files, hashed, watch: native|polling|none}]
terminals:  [{handle, root, cwd, title, pid, clients, exited, scrollback_lines,
              rss_bytes, cpu_permille}]
processes:  [{handle, root, argv, pid, cwd, kept, detached_ms, exited,
              buffered_bytes, rss_bytes, cpu_permille}]
```

`cpu_permille` is thousandths of one core over the interval since the previous
sample (D2), `-1` when there is no previous sample yet. Lists are bounded by the
tables' own caps (`max_terminals`, the process table's limit); `argv` is capped at
spawn (D3). Everything is decoded by a validating `ServerActivityFromJson` on the
client, as the hello is: a field missing or mistyped reads as not reported, never
as a crash or a wrong number.

### D2. CPU is a delta between two requests; one `/proc` walk per request
`server::ProcessActivity` keeps, per pid it reported last time, the `utime+stime`
ticks and the monotonic time of the sample. On a request it walks `/proc` once
(every numeric directory: `stat` for ppid, ticks and RSS pages), builds the
parent map, and for each shell pid (terminals) and process pid sums the subtree —
`watch` under `bash` under the pty, a compiler under `make` under a task. CPU share
is the tick delta over the time delta, or `-1` on the first request for that pid.
Entries whose pid is gone are dropped from the sample map. The walk is a few
hundred small reads; at the view's two-second cadence that is below what the port
sampler does already, and nothing runs when no client asks.

*Alternative:* a sampling thread with a ring of samples, so the first request has
a figure. Rejected: it runs when nobody looks, which is most of a server's life.

### D3. The tables keep what the listing needs
`ProcessTable::Process` gains `root` (the connection's at spawn) and `argv`,
stored as the first 8 arguments each cut to 256 bytes (the listing's row, not a
transcript); `TerminalTable::Terminal` gains `root` (the same field
`remote-terminal-restore` § 6.1 adds — whichever lands first adds it, the other
uses it). Both tables gain `List(...)` under their own mutex, returning plain
structs the server turns into JSON outside the lock.

### D4. The client surface is a contextual sidebar view on the item surface
`SidebarViewSpec{"host", "Host", SidebarMode::Host, contextual=true}`, shown — as
Changed on Host is — only while a host is connected. Rows are
`plugin::PluginHost::SidebarItem`s built by `RemoteHostService::HostRows(host)`:
four depth-0 group rows (`Server`, `Workspaces`, `Terminals`, `Processes`,
collapsible, with the totals in `detail`) and depth-1 rows under each; `id` carries
`kind:handle`, `label` the name (`bash · ~/repo`, `clangd`, the root), `detail` the
figures (`3.2% · 41 MB · 2 clients`, `kept · 12 min · 0.0% · 180 MB`). Figures
are formatted once per refresh in the service, never at paint time — the item
surface paints precomposed strings, as it does for every other mode.

Enter on a terminal row: the tab that holds that handle is focused (the panel
service maps handles to panes through `TerminalSession::host_identity()`,
`remote-terminal-restore` D3), else the terminal is attached in a new tab (the
restore change's cold attach). Enter on a process or workspace row: nothing (the
row is information). Delete on a terminal or process row ends it — `term/close`
(refused by the server while another client is attached, which the row reports as
such) or `proc/signal` SIGTERM then `proc/release` — after a confirm prompt when
it is running; the Server row's Enter runs `Remote: Stop Host Server`'s existing
confirm. All through `ActionId`s so the palette, the control channel and tests
reach them: `RemoteEndHostTerminal <host> <handle>`, `RemoteEndHostProcess <host>
<handle>`.

*Alternative 1:* an Output channel refreshed with text. Rejected: no rows, no
actions, and the Output panel is the terminal's neighbour, which the view wants to
focus. *Alternative 2:* an overlay like Settings. Rejected: a modal surface for
something a user watches while working; the sidebar coexists with the terminal.

### D5. Refresh only while visible, through the existing host-job queue
Showing the view starts a two-second cadence: a `work_` job calls `server/status`
on the host's client (`RemoteServerClient::Call`, as `ShowLog` does), the result
is decoded on that thread and posted to the UI mailbox, where `HostRows` rebuilds
the rows and the sidebar refreshes. Hiding the view, switching it to another mode,
or the connection leaving `Ready` stops the cadence; the next show restarts it and
asks at once. No timer runs while the view is hidden, and a request already in
flight is not duplicated (the job queue is serial).

### D6. `Remote: Show Status` opens the view; the text answer stays for headless use
`ActionId::RemoteShowStatus` shows the Host view for the chosen host instead of a
toast. `StatusText()` stays as the control channel's answer (`--control` has no
sidebar) and gains one line per host with the server's totals from its last
sample. The status bar's remote segment opens the view on click (today it has no
click action), through `StatusBarService` as the architecture notes require.

### D7. `microide-server status` prints a table
The CLI prints the same account as a fixed-width table — server line, then one
block per workspace with its terminals and processes — and `--json` prints what it
prints today. Two requests a second apart give the CPU column its figure; the CLI
does that itself so a user never sees `-1`.

## Risks / Trade-offs

- [A `/proc` walk on a host with thousands of processes] → Still one `readdir` and
  one small `read` per pid, on request only; a `status` of such a host is a
  fraction of what one `git status` costs there. If measured otherwise, restrict
  the walk to the subtrees' pids by reading `/proc/<pid>/task/*/children`.
- [Attributing a process that re-parented to init] → Lost from the subtree sum;
  the figure under-reports and the row says what it sums ("shell and children").
  Daemonized processes are not microide's to show.
- [Two clients ask at once] → Each request gets its own delta against the shared
  previous sample; a very short interval reads as 0% or noisy. Acceptable at a
  two-second cadence; the CLI uses one second deliberately.
- [The view competes with Changed on Host for the contextual slot] → Both are
  contextual views of the same host; they list side by side, as Problems and Tests
  do.

## Migration Plan

Additive on the wire and in the session (the view's visibility persists as any
sidebar mode does). An older server lacks the keys and the view shows the
connection-level rows only, with "not reported" where figures would be.

## Open Questions

- Whether the view should also list the **client's** cost per host (the mirror's
  object store size, `remote.object_store_budget`) as a fifth group. It is one
  more row and the number exists; proposed yes if it costs no new accounting.
