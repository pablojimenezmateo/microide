## Context

The server side of terminal persistence is shipped. `TerminalTable` keeps each host
terminal past its connection (`Detach` on a lost link), resumes it with a cold frame
on `term/attach` (screen plus prefetched history), keeps an exited shell's output
until it is seen, and ends a terminal only on `term/close` from its last client. A
dropped link is already handled on the client too: `RemoteConnection::Replace` warm
-reattaches every tracked `RemoteTerminalChannel` from its own resume point.

What is missing is entirely on the client side of a *quit*, investigated 2026-10-27:

| gap | where |
| --- | --- |
| Remote project panes fail to start before `Ready`, so they are dropped and `terminal_layout_to_revive` is consumed | `RemoteTerminalChannel::Open` returns null on a disconnected client; `make_started_pane` (`WorkspaceShellTerminalTabs.cpp`) returns null; `TerminalPanelService::OpenTerminalsForProjectInit` `continue`s |
| The session has no host identity; Open Terminal on Host panes are skipped | `TerminalPanelService::CaptureLayout`, `TerminalLayoutRecord.h`, `WorkspacePersistenceBinaryFormatSessions.cpp` |
| Close-project kills the shells; quit kills or orphans them, depending on order | `WorkspaceShell::CloseProject` runs `ProjectCatalogService::Close` (terminals destroyed, `term/close` over a live link) *before* `RemoteHostService::ProjectClosed`; on quit `glue_` (connections) is destroyed before `context_` (terminals) |
| Cold attach is unused, and the host's terminals cannot be listed | `RemoteTerminalChannel::Attach` has only test callers; no `term/list` |

`Remote: Open Terminal on Host…` already queues opens until `Ready`
(`RemoteHostService::Host::pending_terminals`, `OpenPendingTerminals`). The project
path has no equivalent.

## Goals / Non-Goals

**Goals:**
- Quit, then reopen, shows the same running host shell, its screen and its history.
- A remote project always has its terminal tab from the moment it opens.
- Host terminal lifetime is decided by explicit steps, never by destructor order.
- A host terminal no tab claims is visible and can be attached or closed.

**Non-Goals:**
- Local terminal process persistence. A local shell is our child and dies with us;
  VS Code needs a pty-host daemon for that, and local revive stays layout-only.
- Handing ptys to a restarted server (`SCM_RIGHTS`, § 6.12, Phase 3).
- An idle reaper for detached terminals. By design (§ 6.12 "Lifetime") a running
  terminal keeps its workspace open indefinitely. The LSP keep-grace does not apply.
  Discoverability through `term/list` is the answer to orphans, not a timer.
- The bottom-panel `+` paint bug, which is fixed separately.

## Decisions

### D1. A pending pane is a real pane with an unstarted session
`make_started_pane` grows a third outcome. On a remote launcher whose connection is
not `Ready`, it returns a pane whose `TerminalSession` is not started and carries a
`PendingHostStart` (cwd, command, and optional `{epoch, handle}`). The body draws a
one-line status from the connection state, precomposed in `RenderViewModelBuilder`
(the render TU stays view-model-only and allocation-free). On the project's first
`Ready` (`RemoteHostService` `project_reconnected`, which already fires on the
first `Ready`) and on a host's `Ready` (where `OpenPendingTerminals` runs today), the
shell walks the panes of that root or host and starts or attaches each pending one.
`OpenPendingTerminals`' counter becomes these pending panes, so both entry points
share one mechanism.

*Alternative:* hold `terminal_layout_to_revive` and build the panes on `Ready`.
Rejected: the panel is empty until then (the reported symptom), the `+` still drops
panes, and the tab strip would reshape under the user.

### D2. Detach is an explicit verb; destruction keeps meaning close
`TerminalSession` gains `DetachFromHost()`, which drops the channel after telling it
to `Detach()`. That unregisters the handle and stops frames, but sends no
`term/close`. `TerminalPanelService::DetachHostTerminals()` calls it on every
host pane. Quit (`WorkspaceLifecycleCoordinator`, before the persistence flush that
writes the session) and `CloseProject` (before `ProjectCatalogService::Close`) call
it first. Destroying a session that was not detached still closes it, so a tab
close, a pane close and every path nobody thought of keep today's safe behaviour.
With `terminal.restore_sessions` off, the detach step is skipped and destruction
closes.

*Alternative:* invert the default (destruction detaches, only an explicit close
closes). Rejected: any missed close path would leak a host shell silently, and a
shell that is killed by mistake is at least loud. `term/list` (D5) is the backstop
for the other direction (a crash).

The session is captured **after** the detach is decided but **before** teardown, so
the recorded handles are the ones left running.

### D3. Host identity is three additive pane records
`TerminalPaneRecord` gains `host_epoch` (string), `host_handle` (u64) and
`host_target` (string; empty means the project's own host). They are written as new
`TerminalPaneTag`s through `PersistedRecordWriter` and read through the existing
`ParseRecordStream`. An old reader skips unknown tags; a new reader treats their
absence as a fresh-shell revive (today's behaviour). Decode bounds: the epoch length
is capped as in `HelloReplyFromJson` (`kMaxEpochBytes`), handle 0 means none, and
the target is validated by `ParseRemoteHostTarget` at revive time, not trusted at
decode. A forged session can at most ask this user's own server to attach a
terminal this user owns. That is the same authority as the user running
`microide-server attach`, so no new trust is extended.

`CaptureLayout` stops skipping launcher-bound panes. A host pane's cwd stays the
mirror path for a project pane and the host path for a terminal-only pane, as
`PaneWorkingDirectory` already distinguishes.

### D4. Attach-or-fallback on Ready, reported once per host
On `Ready`, for each pending pane with `{epoch, handle}`: if
`hello().daemon_epoch == epoch`, call `RemoteTerminalChannel::Attach(client,
handle, session, …)` with no resume point, so the first frame is a cold one. A
refused attach (`term/attach` replies false), or a mismatched epoch, falls back to
`Open` in the recorded cwd. Fallbacks are counted per host and reported in one
notification row (`remote.terminals.<host>`): "2 terminals on build-box could not be
reattached — the host server restarted", or "…they are no longer running". The
reason is `epoch` versus `refused`. The server's reported session-survival policy
(`KillUserProcesses`) is appended when it explains the loss. The attach is
asynchronous like `Open`; nothing waits on the shell thread.

### D5. `term/list` is additive, and offered after the session's claims
Request `term/list {}` returns `{terminals: [{handle, root, cwd, title, clients,
exited, last_output_ms}]}`, built under the `TerminalTable` mutex with counts capped
(as `MaxSessions` caps opens). It is sent after the D4 attaches, so a terminal this
window is about to claim is never offered. The client then filters to `clients ==
0`, the project's root or the empty root for a terminal-only host, and not among
the handles it just attached. An unknown-method error yields an empty list, which
makes the method additive with no wire bump (`kProtocolVersion` stays 3). The row's
Attach opens each terminal in a new tab via the same cold attach; Close sends
`term/close`.

## Risks / Trade-offs

- [Host shells accumulate when a user never reopens] → Each is listed by
  `term/list`, surfaced on the next connect and in `Remote: Show Status`, and ended
  by `Remote: Stop Host Server`. Users who never want this set
  `terminal.restore_sessions` off. A reaper is deliberately not added (Non-Goals).
- [Quit ordering regresses again later] → A test drives quit through
  `WorkspaceShell` destruction against the fake-ssh host and asserts the server still
  lists the terminal with zero clients. Close-project gets the same test.
- [Two windows reopen the same session] → Both attach. The server already shares one
  pty at the smaller size, so this is the existing two-client case, not a conflict.
- [Epoch collision across a host reinstall] → Epochs are unique per server process.
  A false match can only attach the handle number of a different terminal of the same
  user, and the content shown is that terminal's own.
- [A pending pane receives input] → Input to a pending pane is dropped (it is not
  queued into a shell that may become a different one on fallback). The body says
  so.
- [Server started on demand exits with a terminal-only session's terminals] → Verify
  (task 1.2) that a terminal with no workspace keeps an on-demand server alive. If
  not, fix the server's exit rule; the design text already says live terminals hold
  it open.

## Migration Plan

Additive throughout: old sessions revive as fresh shells, old servers simply offer
no listing, and nothing is renamed. Rollback is reverting the client. Host
terminals left running by a newer client are ordinary `term/attach`-able terminals
to an older one.

## Open Questions

- Should a reattached tab show a one-line rule ("reattached — microide was closed
  for 14 min") the way § 7.4a's reconnect gap rule does? The proposed default is
  yes, reusing the gap rule's styling, but it is a cosmetic call to settle in review.
