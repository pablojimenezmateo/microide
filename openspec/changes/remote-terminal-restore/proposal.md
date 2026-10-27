## Why

A first-time remote user left `watch date` running in a remote project's terminal,
quit microide, reopened it, and found no terminal at all. Design
`dev-docs/design/remote-projects.md` § 6.12 promises the opposite. The host owns the
shell and its scrollback precisely so that "reopening after restarting microide"
shows the same running terminal. Today the server keeps host terminals across a
dropped link, but nothing in the client keeps them across a quit. Four separate gaps
add up to the loss (investigated 2026-10-27):

1. **A remote project's terminals never start at open.** The project initializes
   while its connection is still `Connecting`, so `RemoteTerminalChannel::Open`
   returns null (no connected client) and `make_started_pane` drops the pane.
   `OpenTerminalsForProjectInit` loses both the default terminal and every revived
   pane, and it consumes `terminal_layout_to_revive` as it goes. That is also why
   the user found the panel empty and went looking for a `+`.
2. **The session forgets which host shell a pane was.** `CaptureLayout` records only
   cwd and command, and it skips `Remote: Open Terminal on Host…` panes entirely.
   No host handle and no server epoch are written.
3. **Teardown ends or orphans the shell.** Closing a remote project destroys its
   terminals (each sends `term/close` over a live link, so the host shell is killed)
   *before* `RemoteHostService::ProjectClosed` disconnects. On quit, the outcome
   depends on destructor order (`glue_` is destroyed before `context_`): the shell
   is either killed or left running with nothing pointing at it.
4. **Nothing reattaches.** `RemoteTerminalChannel::Attach` (a cold attach: the
   screen plus prefetched scrollback) has no production caller, and there is no
   `term/list`, so a client cannot discover the host terminals nobody is attached to.

## What Changes

- A remote project's terminals (default, revived or new) are created at once as
  **pending panes** that show the connection state. They start, or reattach, when
  the project's connection first reaches `Ready`. This is the same queue-until-Ready
  pattern `Remote: Open Terminal on Host…` already uses (`pending_terminals`).
- **Quit and close-project detach host terminals; they do not close them.** An
  explicit, ordered detach step runs before teardown, so the result no longer
  depends on destructor order. Closing a terminal tab or pane, or the shell exiting,
  still ends it (`term/close`). With `terminal.restore_sessions` off, quit closes
  host terminals as it does today, so a user who opted out of revival leaves
  nothing behind.
- The session's terminal pane record gains **host identity**: the server epoch, the
  host handle, and, for an Open Terminal on Host pane, the host target. Every new
  field is additive.
- On reopen, each recorded pane **cold-attaches to its host terminal** and gets the
  live screen plus `remote.scrollback_prefetch_lines` of history, with older history
  backfilled as today. If the epoch changed, or the handle is gone or refused, the
  pane starts a fresh shell in the saved directory, and one notification row says
  how many could not be reattached and why.
- **New `term/list` request**: the host's live terminals with handle, workspace
  root, cwd, title, attached-client count, exited flag and last-output time. The
  client uses it on `Ready` to report host terminals that no tab claims, with
  **Attach** and **Close** actions. `Remote: Show Status` lists them too. The method
  is additive: an older server answers "unknown method" and the client simply shows
  no listing, so the wire version does not move.

Out of scope: the separate bottom-panel `+` paint bug (the button is drawn only when
a tab exists, but it is clickable regardless); it is fixed on its own. Also out of
scope: two clients on one pty (already shipped) and handing ptys to a restarted
server (§ 6.12, Phase 3).

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `remote-terminal`: adds requirements for deferred start until the connection is
  `Ready`, detach-not-close on quit and project close, reattach on reopen from
  persisted host identity with a visible fallback, and `term/list` discovery of
  host terminals nobody is attached to. The existing requirements (term/close
  semantics, authoritative host scrollback, exit retained until seen) are unchanged.

## Impact

- `src/workspace/services/TerminalPanelService.*`: pending panes, capture and
  revive of host records, and a `DetachHostTerminals()` step.
- `src/workspace/state/TerminalLayoutRecord.h` and
  `src/workspace/persistence/WorkspacePersistenceBinaryFormatSessions.cpp`: new
  additive `TerminalPaneTag` records (epoch, handle, host target), plus fuzz corpus
  entries.
- `src/terminal/TerminalSession*`: a host-mode detach that releases the channel
  without `term/close`, and a pending (not yet started) state.
- `src/project/remote/RemoteTerminalChannel.*`, `RemoteProcessLauncher.*`: a
  production cold-attach path, a detach that is not a close, and the `term/list`
  client.
- `src/server/TerminalTable.*`, `src/server/RemoteServer.cpp`,
  `src/project/remote/RemoteProtocol.h`: `term/list`.
- `src/workspace/services/RemoteHostService.*` and
  `src/workspace/shell/WorkspaceShellTerminalTabs.cpp`: on `Ready`, run the pending
  panes and the attach-or-fallback, plus the unclaimed-terminals row.
- Quit (`WorkspaceLifecycleCoordinator`) and close-project
  (`WorkspaceShell::CloseProject`): the detach step, ordered before teardown.
- Tests: unit (panel service, persistence round trip), kernel (server `term/list`,
  a terminal surviving a connection that went away without `term/close`), fake-ssh
  end to end (quit, then reopen, then the same handle and screen), and a
  `qa-remote` scenario with a long-running command.
- Docs: `remote-projects.md` § 6.12/§ 7.4a "as built", `active-work.md` § 3 (which
  currently says process persistence is a host-daemon feature only), and the
  `TerminalLayoutRecord.h` comment that says processes are not kept.
