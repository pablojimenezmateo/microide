## Why

A first-time remote user left `watch date` running in a remote project's terminal,
quit microide, reopened it, and found no terminal at all. Design
`dev-docs/design/remote-projects.md` § 6.12 promises the opposite. The host owns the
shell and its scrollback precisely so that "reopening after restarting microide"
shows the same running terminal. Today the server keeps host terminals across a
dropped link, but nothing in the client keeps them across a quit. Four separate gaps
add up to the loss (investigated 2026-10-27, re-verified against the code before
this revision):

1. **A remote project's terminals never start at open.** The project initializes
   while its connection is still `Connecting`, so `RemoteTerminalChannel::Open`
   returns null (`!client->connected()`), `make_started_pane`
   (`WorkspaceShellTerminalTabs.cpp`) returns null, and
   `TerminalPanelService::OpenTerminalsForProjectInit` `continue`s past the pane.
   It loses both the default terminal and every revived pane, and it consumes
   `terminal_layout_to_revive` as it goes. That is why the user found the panel
   empty and went looking for a `+`. `Remote: Open Terminal on Host…` does not have
   this problem only because `RemoteHostService::Host::pending_terminals` counts the
   request and `OpenPendingTerminals` opens it on `Ready`; the project path has no
   equivalent.
2. **The session forgets which host shell a pane was.** `CaptureLayout` records only
   cwd and command, and it skips `Remote: Open Terminal on Host…` panes entirely
   (`pane.launcher != nullptr` → dropped). No host handle and no server epoch are
   written, and nothing on the client remembers them either: the handle lives only
   in `RemoteTerminalChannel::handle_`, the epoch only in the client's `HelloReply`.
3. **Teardown is inconsistent.** *Quit* ends in `quick_exit()`
   (`LifecycleCoordinator::Shutdown` deliberately skips terminal teardown), so no
   `term/close` is ever sent: the ssh child dies with the process, the server's
   `TerminalTable::Detach` keeps the terminal, and the shell is **orphaned** — kept
   running, with nothing pointing at it. *Close-project* does the opposite:
   `WorkspaceShell::CloseProject` runs `ProjectCatalogService::Close` (the state is
   destroyed, every `TerminalSession::Stop` sends `term/close` over the still-live
   link, so the shell is **killed**) *before* `RemoteHostService::ProjectClosed`
   disconnects. The earlier draft of this proposal said quit "kills or orphans
   depending on destructor order"; it does not — no destructor runs. So the quit
   side needs no new detach step, only the identity record (gap 2) and, for a user
   who opted out of revival (`terminal.restore_sessions` off), an explicit close
   that today does not happen: that user leaks a host shell on every quit.
4. **Nothing reattaches, and nothing lists.** `RemoteTerminalChannel::Attach` (a
   cold attach: the screen plus prefetched scrollback) has only test callers
   (`tests/RemoteTerminalTests.cpp`), and there is no `term/list`, so a client
   cannot discover the host terminals nobody is attached to — including every one
   that gap 3 has been orphaning.

Two things the earlier draft wanted to verify are already true and need no work:
the server's idle rule counts live terminals (`RemoteServer::Idle()` requires
`terminals_->LiveCount() == 0`, and `remote-server-daemon` says so), so a
terminal-only session keeps an on-demand server alive; and the bottom-panel `+`
paint bug shipped separately (`d8e8df6c7`).

## What Changes

- A remote project's terminals (default, revived or new) are created at once as
  **pending panes**: a real pane in the strip whose session shows the connection's
  state as one status line, started — or reattached — when the project's connection
  first reaches `Ready`. `Remote: Open Terminal on Host…` moves onto the same
  mechanism and its `pending_terminals` counter goes away: one queue-until-Ready
  path instead of a counter for hosts and nothing for projects.
- **Close-project detaches host terminals; it does not close them.** An explicit
  `DetachHostTerminals()` step runs before the project state is torn down, so the
  outcome no longer depends on the order of two unrelated calls. Quit already
  leaves them running (gap 3) and stays as it is. With `terminal.restore_sessions`
  off, both quit and close-project **close** host terminals, so a user who opted
  out of revival leaves nothing behind (today quit leaks). Closing a terminal tab or
  pane, or the shell exiting, still ends the host terminal (`term/close`).
- The session's terminal pane record gains **host identity**: the server epoch, the
  host handle, and, for an Open Terminal on Host pane, the host target. Every new
  field is additive; an old session revives as fresh shells, as today.
- On reopen, each recorded pane **cold-attaches to its host terminal** and gets the
  live screen plus `remote.scrollback_prefetch_lines` of history, with older history
  backfilled on scroll as today, and a dim `⋯ reattached ⋯` rule under the restored
  history (VS Code's "History restored" line). If the epoch changed, or the server
  refuses the handle, the pane starts a fresh shell in the saved directory.
- **New `term/list` request**: the host's live terminals with handle, workspace
  root, cwd, title, attached-client count and exited flag. The client sends it once
  per `Ready`, after its own reattaches, and reports the terminals no tab claims and
  no other client is attached to.
- **One notification row per host** sums the connect up — *"build-box: 2 terminals
  reattached · 1 started fresh (the host server restarted) · 1 running with no
  tab"* — with **Attach** and **Close** actions for the unclaimed ones. This is the
  § 7.4a "reattached to build-box — 3 terminals still running" row, which was
  designed and never built; building it once covers the reattach report, the
  fallback report and the orphan report instead of three rows. `Remote: Show
  Status` lists the unclaimed terminals too. The method is additive: an older
  server answers `kErrorUnknownMethod` and the client shows no listing, so
  `kProtocolVersion` stays 3.

Out of scope: two clients on one pty (already shipped), handing ptys to a restarted
server (§ 6.12, Phase 3), an idle reaper for detached terminals (§ 6.12 says a
running terminal keeps its workspace open), local terminal process persistence
(VS Code needs a pty-host daemon for that; local revive stays layout-only), and the
§ 7.4a dimmed/`detached` tab badge. One limitation is accepted and recorded as a
TD: a host pane opened with **no project open** has no project session to live in
(`terminal_layout` is a `ProjectSessionTag`, and the no-project state is not
persisted), so it is left running on the host and found by `term/list` on the next
connect rather than revived into a tab.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `remote-terminal`: adds requirements for deferred start until the connection is
  `Ready`, detach-not-close on project close (quit already detaches by exiting),
  close-on-opt-out, reattach on reopen from persisted host identity with a visible
  fallback, and `term/list` discovery of host terminals nobody is attached to,
  reported in one per-host row. The existing requirements (term/close semantics,
  authoritative host scrollback, exit retained until seen) are unchanged.

## Impact

- `src/terminal/TerminalHostChannel.h`, `TerminalSession.h`, `TerminalSessionHost.cpp`:
  `Detach()` on the channel (no `term/close`), `HostIdentity` on the channel and
  retained by the session across `DetachFromHost()`, the pending (unstarted, status
  line shown) state, and one shared status-line helper replacing the three
  char-by-char builders (`StartOnHost` failure, `HostConnectionLost`, `MakeGapLine`).
- `src/project/remote/RemoteTerminalChannel.*`, `RemoteProcessLauncher.*`,
  `RemoteConnection.h`: production cold attach, `Detach`, `identity()`, a
  `Ready()` query on `HostTerminalSource`, and the `term/list` client call.
- `src/server/TerminalTable.*`, `src/server/RemoteServer.cpp`,
  `src/project/remote/RemoteProtocol.h`: the terminal's workspace root recorded at
  open, `TerminalTable::List`, `term/list`.
- `src/workspace/state/TerminalLayoutRecord.h`,
  `src/workspace/persistence/WorkspacePersistenceBinaryFormatSessions.cpp`: three
  additive `TerminalPaneTag`s, decode bounds, fuzz corpus entries.
- `src/workspace/services/TerminalPanelService.*`: pending panes, `StartPendingPanes`,
  `DetachHostTerminals`/`CloseHostTerminals`, capture and revive of host identity,
  and `OpenTerminal`/`OpenTerminalOn` folded onto one push-tab helper.
- `src/workspace/services/RemoteHostService.*`,
  `src/workspace/shell/WorkspaceShellTerminalTabs.cpp`: on `Ready`, start or attach
  the pending panes, send `term/list`, raise the per-host summary row; two new
  `ActionId`s (`RemoteAttachHostTerminals`, `RemoteCloseHostTerminals`).
- `WorkspaceShell::CloseProject` and `LifecycleCoordinator::Shutdown`: the detach
  (close-project) and opt-out close (both) steps, ordered before teardown.
- Tests: unit (panel service pending/capture/revive, persistence round trip and
  fuzz), kernel (`term/list`, exited-while-detached retention, a terminal-only
  session holding the server), fake-ssh end to end (project opens with its tab;
  close-project leaves the shell; quit, reopen, same handle and screen; epoch
  fallback; orphan offered), and the `qa-remote` relaunch scenario (`L`).
- Docs: `remote-projects.md` § 6.12/§ 7.4a "as built", `active-work.md` § 3,
  `TerminalLayoutRecord.h`'s header comment, `known-tech-debt.md` for the
  no-project host pane.
