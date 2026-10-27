## Context

The server side of terminal persistence is shipped. `TerminalTable` keeps each host
terminal past its connection (`Detach` on a lost link), resumes it with a cold frame
on `term/attach` (screen plus prefetched history), keeps an exited shell's output
until `term/close`, and ends a terminal only on `term/close` from its last client.
`RemoteServer::Idle()` counts live terminals, so a terminal holds an on-demand
server open. A dropped link is already handled on the client too:
`RemoteConnection::Replace` warm-reattaches every tracked `RemoteTerminalChannel`
from its own resume point.

What is missing is on the client side of a *quit* and a *close-project*, verified
2026-10-27 against the code:

| gap | where |
| --- | --- |
| Remote project panes fail to start before `Ready`, so they are dropped and `terminal_layout_to_revive` is consumed | `RemoteTerminalChannel::Open` returns null on a disconnected client; `make_started_pane` returns null; `OpenTerminalsForProjectInit` `continue`s |
| The session has no host identity; Open Terminal on Host panes are skipped | `TerminalPanelService::CaptureLayout`, `TerminalLayoutRecord.h`, `WorkspacePersistenceBinaryFormatSessions.cpp` |
| Close-project kills the shells (`term/close` over a live link before `ProjectClosed`); quit orphans them (`Shutdown` → `quick_exit`, no teardown, no `term/close`) | `WorkspaceShell::CloseProject`, `LifecycleCoordinator::Shutdown` |
| Cold attach is unused; the host's terminals cannot be listed; § 7.4a's "reattached — N terminals still running" row was never built | `RemoteTerminalChannel::Attach` has only test callers; no `term/list`; `RemoteHostService` |

Priorities applied throughout, in the repo's order: speed (nothing on the shell
thread waits on the host; every attach is asynchronous, as `Open` is), correctness
(a shell is never killed by accident and never silently leaked), then CPU/memory
(no polling, no reaper, no per-frame string building, one request per `Ready`), and
less code than today where two paths did one job.

## Goals / Non-Goals

**Goals:**
- Quit, then reopen, shows the same running host shell, its screen and its history.
- A remote project always has its terminal tab from the moment it opens.
- Host terminal lifetime is decided by explicit steps, never by call order.
- A host terminal no tab claims is visible and can be attached or closed.
- One mechanism where there were two (hosts' `pending_terminals` counter versus
  nothing for projects; three bracketed status-line builders; two push-a-tab bodies).

**Non-Goals:**
- Local terminal process persistence (VS Code needs a pty-host daemon; local revive
  stays layout-only).
- Handing ptys to a restarted server (`SCM_RIGHTS`, § 6.12, Phase 3).
- An idle reaper for detached terminals. By design (§ 6.12 "Lifetime") a running
  terminal keeps its workspace open indefinitely. Discoverability through
  `term/list` is the answer to orphans, not a timer.
- § 7.4a's dimmed tab with a `detached` badge while a link is down.
- Reviving a host pane that was opened with no project open (see D3).

## Decisions

### D1. A pending pane is a real pane with an unstarted session, and its status is a session line
`HostTerminalSource` gains `bool Ready() const` (the launcher's connection has a
connected client). `make_started_pane` grows a third outcome: on a host launcher
that is not `Ready`, it returns a pane whose `TerminalSession` holds a
`PendingHostStart{cwd, command, identity}` and is not started. The session shows
its state as ONE line in its own buffer — `[connecting to build-box…]`, then
`[authenticate to build-box in the terminal below]`, `[starting the host server…]`
— rewritten by `TerminalSession::SetPendingStatus(text)` only when the connection's
state changes (the host service already receives exactly those events), never per
frame. Input to a pending pane is dropped (it must not be queued into a shell that
may become a different one on fallback), and the line says so.

On the connection's first `Ready` — `RemoteHostService` already computes it for a
project (`came_back` in `ApplyProject`, which fires `project_reconnected`) and for a
host (`Apply`'s `State::Ready`, where `OpenPendingTerminals` runs today) — the
service asks the shell to `StartPendingPanes(launcher)`: every pending pane bound
to that launcher starts (D4). `Host::pending_terminals` and `OpenPendingTerminals`
are deleted; `OpenTerminalOnHost` opens a pending pane at once through the same
`open_terminal` operation, so both entry points share the one mechanism and the
host terminal's tab also appears immediately rather than on `Ready`. On
`Disconnected`/`Offline` the pending panes show the error line and relaunch with
Enter through the existing `relaunch_pane` (they are `!running()`, which is what
`RelaunchActivePane` keys on).

`TerminalPanelService` acts on `context_.current_project_state` only, so pending
panes of an **inactive** project are not reachable from a `Ready` for its root.
They start when that project is next activated: `ActivateProjectState` (which
already runs `OpenTerminalsForProjectInit` for a fresh state) also runs
`StartPendingPanes` when the project's launcher is `Ready`. Event-driven both ways,
no polling, no reaching into stored states.

*Alternative 1:* hold `terminal_layout_to_revive` and build the panes on `Ready`.
Rejected: the panel is empty until then (the reported symptom), the `+` still drops
panes, and the tab strip would reshape under the user.
*Alternative 2 (the earlier draft):* precompose the status line in
`RenderViewModelBuilder` and paint it from a new view-model field. Rejected: it is
a new paint path for one line, it rebuilds a string on every frame the pane is
visible unless cached, and the session already draws its own status lines for the
two neighbouring states (`[could not open a terminal on the host…]`,
`[connection to the host lost…]`). Using the buffer keeps the render TUs and
`RenderViewModelBuilder` untouched and the pane's paint, hover and selection paths
unchanged.

*Cleanup folded in:* those two builders and `MakeGapLine` each push a line glyph by
glyph; the pending status line would be a fourth copy. One
`TerminalSession::AppendStatusLineLocked(text, dim)` replaces all of them.

### D2. Detach is an explicit verb; destruction keeps meaning close; quit stays as it is
`TerminalHostChannel::Detach()` unregisters the handle and stops frames with no
`term/close` (`RemoteTerminalChannel::Detach` = `Close` minus the notify).
`TerminalSession::DetachFromHost()` calls it, retains the channel's `HostIdentity`
(D3) and drops the channel, so the pane reads as `!running()` with the identity
still readable. Destroying a session that was not detached still closes it, so a
tab close, a pane close and every path nobody thought of keep today's safe
behaviour.

`TerminalPanelService::DetachHostTerminals()` runs it on every host pane (project
and Open Terminal on Host), and `CloseHostTerminals()` stops them instead.
`WorkspaceShell::CloseProject` calls one or the other **before**
`ProjectCatalogService::Close` (which persists the entry — reading the retained
identity — and then destroys it); `Detach` when `terminal.restore_sessions` is on,
`Close` when it is off.

Quit needs no detach: `Shutdown` persists the active entry while every channel is
still live and attached (the identity is read from the channel), and `quick_exit`
drops the link, which is a detach on the server. With `terminal.restore_sessions`
off, `Shutdown` gains one step, `CloseHostTerminals()` before `persist_active_project`,
so the opt-out stops leaking (today it orphans a shell per quit). Local terminals are
still not torn down there, as the comment in `Shutdown` explains.

*Alternative:* invert the default (destruction detaches, only an explicit close
closes). Rejected: a missed close path would leak a host shell silently, and a
shell killed by mistake is at least loud. `term/list` (D5) is the backstop for the
other direction (a crash, and the quits that happened before this change).

### D3. Host identity is one struct, read from the channel, kept by the session, written as three pane records
`struct HostIdentity { std::string epoch; std::uint64_t handle; }`.
`TerminalHostChannel::identity()` returns it (`RemoteTerminalChannel`: the client's
`hello().daemon_epoch` and `handle_`; zero handle until `term/open` answers).
`TerminalSession::host_identity()` returns the live channel's, or the one retained
by `DetachFromHost()`; `Stop()` clears it. Nothing is duplicated into the pane
state and nothing is written from the I/O thread.

`TerminalPaneRecord` gains `host_epoch` (string), `host_handle` (u64) and
`host_target` (string; empty = the project's own host), written as three new
`TerminalPaneTag`s through `PersistedRecordWriter` and read by the existing
`ParseRecordStream`. An old reader skips unknown tags; a new reader treats their
absence as today's fresh-shell revive. Decode bounds: epoch ≤ `kMaxEpochBytes`
(64, the hello's own cap), handle 0 = none, and the target is validated by
`ParseRemoteHostTarget` at revive time (the existing command-line guard), not
trusted at decode. A forged session can at most ask this user's own server to
attach a terminal this user owns — the same authority as `microide-server attach`.

`CaptureLayout` stops skipping launcher-bound panes. A host pane's cwd stays the
mirror path for a project pane and the host path for a terminal-only pane, as
`PaneWorkingDirectory` already distinguishes. The record lives in the **project
session** (`ProjectSessionTag::TerminalTab`); a host pane opened while a project is
active is recorded in that project's session and revived with it. A host pane
opened with **no project open** has no session to live in (the no-project state is
not persisted) — it is left running and offered by `term/list` on the next connect.
Recorded as a TD rather than extended here: it needs a per-window terminal record
in the workspace session, which is a format decision of its own.

### D4. Attach-or-fallback on Ready, asynchronous, counted into one row
`StartPendingPanes(launcher)`: for each pending pane bound to it, if the pane has an
identity and `client->hello().daemon_epoch == identity.epoch`, start the session
with `RemoteTerminalChannel::Attach(client, handle, session, …)` and no resume
point, so the first frame is cold (screen plus `remote.scrollback_prefetch_lines`);
the session then appends the dim `⋯ reattached ⋯` rule under the restored history
(the gap rule's styling through the D1 helper; no duration — that would need a
persisted detach time for one cosmetic word). A mismatched epoch, or a refused
`term/attach` (the reply is an error), falls back to `Open` in the recorded cwd —
the channel reports the refusal to the session through the existing
`HostConnectionLost` path, and the panel's fallback is the existing relaunch. The
fallback reason (`epoch` vs `refused`) is counted per host for the D5 row, with the
server's `session_survival.kill_user_processes` appended when it explains the loss.
Nothing waits on the shell thread: the attach reply lands on the I/O thread as
`Open`'s does.

A reattached exited shell (`kRunning` clear in the cold frame) shows its final
output and exit line and relaunches with Enter: the existing exited-pane path.

### D5. `term/list` is additive, offered after the session's claims, and reported in the § 7.4a row
Request `term/list {}` returns `{terminals: [{handle, root, cwd, title, clients,
exited}]}`, built under the `TerminalTable` mutex and bounded by `max_terminals`
(64). `root` is the connection's workspace root at `term/open`, recorded on the
`Terminal` then (today the table does not know it); `cwd` is the session's reported
working directory; `title` the last OSC title; `exited` is `!running()`. No
last-output time: nothing in this change reads it, and it would cost a store per
output chunk on the reader thread.

The client sends it once per `Ready`, after the D4 attaches on the same lane, so a
terminal this window is claiming is never offered, and filters to `clients == 0`,
the project's root (or the empty root for a terminal-only host), and not among the
handles it just attached. `kErrorUnknownMethod` (-32601, what `RemotePeer` answers
for any method it does not know) yields an empty list, so the method is additive
and `kProtocolVersion` stays 3.

The result goes into **one sticky row per host** (`remote.terminals.<host>`): the
§ 7.4a "reattached to build-box" row, which was designed and not built. It sums the
connect — *"build-box: 2 terminals reattached · 1 started fresh (the host server
restarted) · 1 running with no tab"* — and carries **Attach** (each unclaimed
terminal in a new tab via the same cold attach) and **Close** (`term/close` for
each) as `ActionId::RemoteAttachHostTerminals` / `RemoteCloseHostTerminals` with
`{host, handle…}` args. With nothing to say (all reattached, none unclaimed) the
row is a plain transient "Reattached to build-box: 2 terminals". `Remote: Show
Status` appends the unclaimed terminals to its host line.

## Risks / Trade-offs

- [Host shells accumulate when a user never reopens] → Each is listed by
  `term/list`, surfaced on the next connect and in `Remote: Show Status`, and ended
  by Close or `Remote: Stop Host Server`. Users who never want this set
  `terminal.restore_sessions` off, which now closes on quit too. No reaper.
- [Close-project ordering regresses later] → A fake-ssh test asserts the server
  still lists the terminal with zero clients after `CloseProject`, and another that
  a plain tab close ends it.
- [Two processes reopen the same session] → Both attach. The server already shares
  one pty at the smaller size, so this is the existing two-client case.
- [Epoch collision across a host reinstall] → Epochs are unique per server process.
  A false match can only attach the handle number of a different terminal of the
  same user, and the content shown is that terminal's own.
- [A pending pane receives input] → Dropped, and the status line says so.
- [Pending panes of an inactive project never start] → They start on activation
  (D1); a project that is never activated again has nothing on screen to show them
  in anyway, and its recorded identities are untouched for the next open.
- [An old client reopens a session written by this one] → Unknown tags are skipped;
  it revives fresh shells and the host terminals stay orphaned until a new client
  lists them.

## Migration Plan

Additive throughout: old sessions revive as fresh shells, old servers simply offer
no listing, and nothing is renamed. Rollback is reverting the client. Host
terminals left running by a newer client are ordinary `term/attach`-able terminals
to an older one.

## Open Questions

None. The earlier draft's question (a "reattached" rule in the tab) is decided in
D4: yes, dim, without a duration.
