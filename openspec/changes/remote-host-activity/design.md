## Context

The server already answers `server/status` (what `microide-server status` prints;
after `remote-terminal-restore` it lists the terminals and the client decodes it
with `ServerStatusFromJson` through `RemoteServerClient::Status()`), and it
already runs a `PeriodicSampler` (2 s) for `ports/subscribe` that is created by
the first subscriber and destroyed with the last (`RemoteServer::SamplePorts`,
`port_sampler_`). The client already has `Remote: Show Status`, `Remote: Show
Log` (a host job through `RemoteHostService::work_`, shown in an Output channel),
`Remote: Stop Host Server`, and the ports subscription pattern
(`WatchPorts`/`OnHostPorts`: a server push lands on the I/O thread and is posted
to the UI mailbox). The sidebar already has a contextual, host-owned view on the
plugin item surface — Changed on Host (`SidebarMode::HostChanges`, rows built by
`RemoteHostService::HostChangeRows`, Enter routed by
`WorkspaceSidebarCoordinatorActions`); the item surface has no Delete key case.
The app has no general client-side timer: the shell's idle loop wakes for its own
debounce deadlines only.

What is missing is the data (the server keeps no argv per process, no resource
figures) and a surface that is a view rather than a toast.

Priorities, in the repo's order: speed (the view costs nothing until it is opened;
after that one `/proc` walk per tick on the host and one push per tick, no
request/reply round trip), correctness (every figure is attributed to a microide
object — a tab, a project's tool — or not shown), CPU/memory (no sampling while
nobody is subscribed, no per-frame work on the client, no client timer), and
reuse (one item surface, one subscription pattern, one status method, one
decoder, the restore change's action ids).

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
- A client-side refresh timer; a second wire method for the listing.
- macOS hosts (the server is Linux; the client side is portable and shows what the
  server sends).

## Decisions

### D1. `server/status` grows; figures behind `activity: true`; no new listing method
The method exists, the CLI prints it, the restore change made it the terminal
listing, and every field is additive, so the wire version stays 3 and an older
server simply lacks the new keys, which the decoder treats as "not reported".

Shape (new keys only; the restore change's `terminals` rows gain `pid` and
`scrollback_lines`):

```
request:    server/status {activity?: bool}
workspaces: [{root, clients, terminals, processes, files, hashed, watch: native|polling|none}]
terminals:  [{handle, root, cwd, clients, exited, pid, scrollback_lines,
              rss_bytes?, cpu_permille?}]
processes:  [{handle, root, argv, pid, cwd, kept, detached_ms, exited,
              buffered_bytes, rss_bytes?, cpu_permille?}]
server:     {uptime_ms, idle_since_ms, rss_bytes?, cpu_permille?}
```

The `?` fields are present only when `activity` was asked (or in a subscription
push), because they are the ones that read `/proc`; a plain `server/status` —
the restore change's once-per-`Ready` ask, the CLI's `--json` — costs the table
walks and nothing else. `cpu_permille` is thousandths of one core over the
interval since the previous sample (D2), `-1` when there is no previous sample
yet. Lists are bounded by the tables' own caps (`max_terminals`, the process
table's limit); `argv` is capped at spawn (D3). The restore change's
`ServerStatusFromJson` grows the new fields: a field missing or mistyped reads as
not reported, never as a crash or a wrong number; pids and counts are range
checked, argv entries must be strings.

### D2. The server samples on its existing subscription pattern; CPU is a delta between samples
`activity/subscribe {}` replies with `StatusJson(activity = true)` and registers
the connection; the first subscriber creates an `activity_sampler_`
(`PeriodicSampler`, 2 s) exactly as `ports/subscribe` creates `port_sampler_`;
each tick builds `StatusJson(activity = true)` and pushes it as
`activity/changed` to every subscriber; `activity/unsubscribe {}` (a
notification) or the connection closing removes the subscriber, and the last
removal destroys the sampler (`RemoteServer::Shutdown` already stops the port
one first; the activity one joins it). A `server/status {activity: true}` from a
non-subscriber (the CLI) takes a one-shot sample through the same
`ProcessActivity` instance.

`server::ProcessActivity` keeps, per pid it reported last time, the
`utime+stime` ticks and the monotonic time of the sample. A sample walks `/proc`
once (every numeric directory: one read of `stat` for ppid, ticks and RSS pages),
builds the parent map, and for each shell pid (terminals) and process pid sums the
subtree — `watch` under `bash` under the pty, a compiler under `make` under a
task. CPU share is the tick delta over the time delta, or `-1` on the first
sample for that pid. Entries whose pid is gone are dropped from the sample map.
The walk is one small read per process on the box; at 2 s while someone watches
that is what the port sampler already costs, and nothing runs when nobody is
subscribed.

*Alternative 1 (the earlier draft):* the client polls `server/status` on a
2-second timer. Rejected: the app has no timer facility to hang it on (the idle
loop wakes for its own deadlines; a `work_` job that sleeps would block Show Log
and Stop Server behind it), every tick is a request/reply round trip instead of a
push, and the "no sampling thread in the server" goal it served is already
abandoned by the port sampler the server runs for the same reason — while
subscribed, and only then.
*Alternative 2:* a sampling thread with a ring of samples, so the first request
has a figure. Rejected: it runs when nobody looks, which is most of a server's
life; the subscription gives the second sample two seconds in.

### D3. The tables keep what the listing needs
`ProcessTable::Process` gains `root` (the connection's at spawn) and `argv`,
stored as the first 8 arguments each cut to 256 bytes (the listing's row, not a
transcript); `ProcessTable::List()` under its mutex returns plain structs the
server turns into JSON outside the lock. `TerminalTable::Terminal` already has
`root` and `List()` from the restore change; `List()` also returns the shell pid
and the scrollback line count.

### D4. The client surface is a contextual sidebar view on the item surface
`SidebarViewSpec{"host", "Host", SidebarMode::Host, contextual=true}`, shown — as
Changed on Host is — only while a host is connected, and reached from Remote >
Show Status and the palette, never from the view switcher (contextual views are
not in it, and not in the View menu). Rows are `plugin::PluginHost::SidebarItem`s
built by `RemoteHostService::HostRows(host)` from the last `ServerStatus` seen:
four depth-0 group rows (`Server`, `Workspaces`, `Terminals`, `Processes`,
collapsible, with the totals in `detail`) and depth-1 rows under each; `id`
carries `kind:handle`, `label` the name (`bash · ~/repo`, `clangd`, the root),
`detail` the figures (`3.2% · 41 MB · 2 clients`, `kept · 12 min · 0.0% · 180
MB`, `… · 41 MB` while CPU is not reported yet). Figures are formatted once per
push in the service, never at paint time — the item surface paints precomposed
strings, as it does for every other mode.

Enter on a terminal row: the tab that holds that handle is focused (the panel
service maps handles to panes through `TerminalSession::host_identity()`, the
restore change's D3), else `RemoteAttachHostTerminals <host> <handle>` (the
restore change's cold attach in a new tab). Enter on a process or workspace row:
nothing (the row is information). Delete on a terminal row runs
`RemoteCloseHostTerminals <host> <handle>` (the restore change's; `term/close`,
which the server refuses while another client is attached, and the row then says
so); on a process row `RemoteEndHostProcess <host> <handle>` (`proc/signal`
SIGTERM then `proc/release`); both after a confirm prompt when the item is
running. The Server row's Enter runs `Remote: Stop Host Server`'s existing
confirm. The item surface gains one Delete key case, routed through a per-mode
"row end action" that only `SidebarMode::Host` declares; other modes ignore the
key as today. All through `ActionId`s so the palette, the control channel and
tests reach them.

*Alternative 1:* an Output channel refreshed with text. Rejected: no rows, no
actions, and the Output panel is the terminal's neighbour, which the view wants to
focus. *Alternative 2:* an overlay like Settings. Rejected: a modal surface for
something a user watches while working; the sidebar coexists with the terminal.
*Alternative 3:* new `RemoteEndHostTerminal` beside the restore change's
`RemoteCloseHostTerminals`. Rejected: the same verb with the same arguments.

### D5. Subscribe on show, unsubscribe on hide; the push lands through the mailbox
`ShowHost(host)` calls `RemoteServerClient::SubscribeActivity(handler)` on the
host-job queue (as `WatchPorts` calls `SubscribePorts`); the reply and every
`activity/changed` are decoded on the I/O thread and posted with
`mailbox_.PostLatest("remote-activity:" + host, …)` (only the latest matters by
the time the UI runs), where `HostRows` rebuilds the rows and the sidebar
refreshes. Hiding the view, switching it to another mode, or the connection
leaving `Ready` sends `activity/unsubscribe`; a reconnect while the view is
visible resubscribes from `ApplyProject`'s `came_back`, as `WatchPorts` re-runs
there. No request is in flight while hidden and nothing is sent to a hidden view.

### D6. `Remote: Show Status` opens the view; the segment stays the Remote menu's; the text answer stays for headless use
`ActionId::RemoteShowStatus` shows the Host view for the chosen host instead of a
toast. `StatusText()` stays as the control channel's answer (`--control` has no
sidebar) and gains one line per host with the server's totals from the last
`ServerStatus` seen (the restore change's ask, or the view's pushes). The status
bar's remote segment opens the Remote menu (`remote-menu` D2) and is not touched
here: Show Status is that menu's second item, one click away, and a segment with
two meanings is the thing the menu change removed.

### D7. `microide-server status` prints a table
The CLI prints the same account as a fixed-width table — server line, then one
block per workspace with its terminals and processes — and `--json` prints what it
prints today (a plain `server/status`, no figures). For the table it asks
`server/status {activity: true}` twice a second apart so the CPU column has its
figure and a user never sees `-1`.

## Risks / Trade-offs

- [A `/proc` walk on a host with thousands of processes] → Still one `readdir`
  and one small `read` per pid, every 2 s only while someone watches; a sample on
  such a host is a fraction of what one `git status` costs there. If measured
  otherwise, restrict the walk to the subtrees' pids by reading
  `/proc/<pid>/task/*/children`.
- [Attributing a process that re-parented to init] → Lost from the subtree sum;
  the figure under-reports and the row says what it sums ("shell and children").
  Daemonized processes are not microide's to show.
- [A one-shot CLI sample between two sampler ticks] → Each sample's delta is
  against the shared previous sample; a very short interval reads as 0% or noisy.
  Acceptable: the CLI takes its own two samples a second apart, and a subscriber's
  next tick corrects itself.
- [Two clients subscribed] → One sampler, one walk per tick, one push per
  subscriber; the second subscriber is a vector entry.
- [The view competes with Changed on Host for the contextual slot] → Both are
  contextual views of the same host; they list side by side, as Problems and Tests
  do.

## Migration Plan

Additive on the wire and in the session (the view's visibility persists as any
sidebar mode does; on restore it resubscribes once the connection is `Ready`). An
older server lacks the keys and the methods: the view shows the connection-level
rows and the restore change's terminal rows with the figures marked not reported,
and `activity/subscribe` answering `kErrorUnknownMethod` leaves the view static
with no error.

## Open Questions

None. The earlier draft's question (a fifth group for the client's own cost per
host) is decided: no — the mirror's size is a property of the project, not of the
host's activity, and `Remote: Show Status`'s text already names the mirror.
