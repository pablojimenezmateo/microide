## Why

A first-time remote user left `watch date` running in a remote project's terminal,
quit microide, reopened it, and found no terminal at all. Design
`dev-docs/design/remote-projects.md` § 6.12 promises the opposite. The host owns the
shell and its scrollback precisely so that "reopening after restarting microide"
shows the same running terminal. Today the server keeps host terminals across a
dropped link, but nothing in the client keeps them across a quit. Four separate gaps
add up to the loss (investigated 2026-10-27, re-verified against the code before
this revision; written against the tree after `remove-host-terminal-command`):

1. **A remote project's terminals never start at open.** The project initializes
   while its connection is still `Connecting`, so `RemoteTerminalChannel::Open`
   returns null (`!client->connected()`), `make_started_pane`
   (`WorkspaceShellTerminalTabs.cpp`) returns null, and
   `TerminalPanelService::OpenTerminalsForProjectInit` `continue`s past the pane.
   It loses both the default terminal and every revived pane, and it consumes
   `terminal_layout_to_revive` as it goes. That is why the user found the panel
   empty and went looking for a `+`. Nothing queues a terminal until `Ready`.
2. **The session forgets which host shell a pane was.** `CaptureLayout` records only
   cwd and command. No host handle and no server epoch are written, and nothing on
   the client remembers them either: the handle lives only in
   `RemoteTerminalChannel::handle_`, the epoch only in the client's `HelloReply`.
3. **Teardown is inconsistent.** *Quit* ends in `quick_exit()`
   (`LifecycleCoordinator::Shutdown` deliberately skips terminal teardown), so no
   `term/close` is ever sent: the ssh child dies with the process, the server's
   `TerminalTable::Detach` keeps the terminal, and the shell is **orphaned** — kept
   running, with nothing pointing at it. *Close-project* does the opposite:
   `WorkspaceShell::CloseProject` runs `ProjectCatalogService::Close` (the state is
   destroyed, every `TerminalSession::Stop` sends `term/close` over the still-live
   link, so the shell is **killed**) *before* `RemoteHostService::ProjectClosed`
   disconnects. The earlier draft of this proposal said quit "kills or orphans
   depending on destructor order"; it does not in the shipping binary — no
   destructor runs. But `quick_exit_on_shutdown` is off in the test and perf
   binaries (`AppStartupOptions.h`), where the destructors DO run and the same quit
   kills every host shell; so "quit detaches" is today true only by the accident
   of how the process ends, and a user who opted out of revival
   (`terminal.restore_sessions` off) has no close at all: that user leaks a host
   shell on every quit.
4. **Nothing reattaches, and nothing lists.** `RemoteTerminalChannel::Attach` (a
   cold attach: the screen plus prefetched scrollback) has only test callers
   (`tests/RemoteTerminalTests.cpp`), and `server/status` reports terminals only
   as counts (`RemoteServer::StatusJson`), so a client cannot discover the host
   terminals nobody is attached to — including every one that gap 3 has been
   orphaning.

Two things the earlier draft wanted to verify are already true and need no work:
the server's idle rule counts live terminals (`RemoteServer::Idle()` requires
`terminals_->LiveCount() == 0`, and `remote-server-daemon` says so), so a
terminal-only session keeps an on-demand server alive; and the bottom-panel `+`
paint bug shipped separately (`d8e8df6c7`).

## What Changes

- A remote project's terminals (default, revived or new) are created at once as
  **pending panes**: a real pane in the strip whose session shows the connection's
  state as one status line, started — or reattached — when the project's connection
  first reaches `Ready`.
- **Quit and close-project detach running host terminals; they do not close
  them.** One explicit step — detach, or close when `terminal.restore_sessions` is
  off — runs over the closing project's terminals before its state is persisted
  and torn down, and over **every** open project's at quit (the active state and
  the catalog's stored ones) before `Shutdown` persists, so the outcome depends on
  neither call order nor `quick_exit`. A terminal whose shell has **already
  exited** while this client watched is closed by both (nothing to keep, and a
  dead buffer would hold an on-demand server open). With
  `terminal.restore_sessions` off, both **close** every host terminal, so a user
  who opted out of revival leaves nothing behind (today quit leaks). Closing a
  terminal tab or pane, or the shell exiting, still ends the host terminal
  (`term/close`).
- The session's terminal pane record gains **host identity**: the server epoch and
  the host handle. Both fields are additive; an old session revives as fresh
  shells, as today.
- On reopen, each recorded pane **cold-attaches to its host terminal** and gets the
  live screen plus `remote.scrollback_prefetch_lines` of history, with older history
  backfilled on scroll as today, and a dim `⋯ reattached ⋯` rule under the restored
  history (VS Code's "History restored" line). If the epoch changed, or the server
  refuses the handle, the pane starts a fresh shell in the saved directory.
- **`server/status` lists the terminals.** Its reply gains a `terminals` array —
  handle, workspace root, cwd, attached-client count and exited flag — beside the
  counts it reports today, and the per-workspace `terminals` count stops being a
  hard-coded zero. No new method: the CLI's `microide-server status` gains the
  listing for free, and `remote-host-activity` extends the same array rather than
  adding a second one. The client sends it once per `Ready`, after its own
  reattaches, and reports the terminals no tab claims and no other client is
  attached to. (No title: the server-side session keeps no window title, and the
  row's identity is its directory.)
- **One notification row per host** sums the connect up — *"build-box: 2 terminals
  reattached · 1 started fresh (the host server restarted) · 1 running with no
  tab"* — with **Attach** and **Close** actions for the unclaimed ones. This is the
  § 7.4a "reattached to build-box — 3 terminals still running" row, which was
  designed and never built; building it once covers the reattach report, the
  fallback report and the orphan report instead of three rows. `Remote: Show
  Status` lists the unclaimed terminals too. The reply is additive: an older
  server's reply has no `terminals` key and the client shows no listing, so
  `kProtocolVersion` stays 3.

Out of scope: two clients on one pty (already shipped), handing ptys to a restarted
server (§ 6.12, Phase 3), an idle reaper for detached terminals (§ 6.12 says a
running terminal keeps its workspace open), local terminal process persistence
(VS Code needs a pty-host daemon for that; local revive stays layout-only), and the
§ 7.4a dimmed/`detached` tab badge.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `remote-terminal`: adds requirements for deferred start until the connection is
  `Ready`, an explicit detach-not-close on quit and on project close,
  close-on-opt-out and close-of-exited, reattach on reopen from persisted host
  identity with a visible fallback, and discovery of host terminals nobody is
  attached to through `server/status`, reported in one per-host row. The existing
  requirements (term/close semantics, authoritative host scrollback, exit retained
  until seen) are unchanged.

## Impact

- `src/terminal/TerminalHostChannel.h`, `TerminalSession.h`, `TerminalSessionHost.cpp`:
  `Detach()` on the channel (no `term/close`), `HostIdentity` on the channel and
  retained by the session across `DetachFromHost()`, the pending (unstarted, status
  line shown) state, and one shared status-line helper replacing the three
  char-by-char builders (`StartOnHost` failure, `HostConnectionLost`, `MakeGapLine`).
- `src/project/remote/RemoteTerminalChannel.*`, `RemoteProcessLauncher.*`,
  `RemoteConnection.h`: production cold attach, `Detach`, `identity()`, a
  `Ready()` query on `HostTerminalSource`.
- `src/server/TerminalTable.*`, `src/server/RemoteServer.cpp`,
  `src/project/remote/RemoteProtocol.{h,cpp}`, `RemoteServerClient.*`: the
  terminal's workspace root recorded at open, `TerminalTable::List`, the
  `terminals` array in `StatusJson` (and the per-workspace count), a validating
  `ServerStatus` decoder (`ServerStatusFromJson`, every field untrusted as the
  hello's are) and `RemoteServerClient::Status()`.
- `src/workspace/state/TerminalLayoutRecord.h`,
  `src/workspace/persistence/WorkspacePersistenceBinaryFormatSessions.cpp`: two
  additive `TerminalPaneTag`s, decode bounds, fuzz corpus entries.
- `src/workspace/services/TerminalPanelService.*`: pending panes, `StartPendingPanes`,
  one `ReleaseHostTerminals(ProjectWorkspaceState&, Release)` helper (detach or
  close) used by close-project and by quit, capture and revive of host identity.
- `src/workspace/services/RemoteHostService.*`,
  `src/workspace/shell/WorkspaceShellTerminalTabs.cpp`: on `Ready`, start or attach
  the pending panes, ask `server/status`, raise the per-host summary row; two new
  `ActionId`s (`RemoteAttachHostTerminals`, `RemoteCloseHostTerminals`, each
  `<host> <handle>…`; `remote-host-activity` reuses both).
- `WorkspaceShell::CloseProject` and `LifecycleCoordinator::Shutdown`: the release
  step, ordered before persistence and teardown, over the closing project and over
  every open project respectively.
- Tests: unit (panel service pending/capture/revive, persistence round trip and
  fuzz), kernel (`server/status` terminals across two roots, exited-while-detached
  retention, a terminal-only session holding the server), fake-ssh end to end
  (project opens with its tab;
  close-project leaves the shell; quit, reopen, same handle and screen; epoch
  fallback; orphan offered), and a `qa-remote` group `T` of fourteen host-checked
  scenarios (tasks § 8: survive a quit, X and Ctrl+D leave nothing, server
  restarted, finished while closed, crash orphan found, opt-out, close-project,
  `+` while connecting, deep scrollback, a second client, Show Status, tab close
  ends the process, all under the slow link).
- Docs: `remote-projects.md` § 6.12/§ 7.4a "as built", `active-work.md` § 3,
  `TerminalLayoutRecord.h`'s header comment.
- Sequencing: after `remove-host-terminal-command`.
