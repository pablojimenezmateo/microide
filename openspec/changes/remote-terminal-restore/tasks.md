## 1. Pin the current behaviour with failing tests

- [ ] 1.1 Fake-ssh end-to-end test: open a remote project whose link becomes `Ready` after a delay; assert a terminal tab exists from project open and that the same tab gets a host prompt once `Ready` (fails today: the pane is dropped)
- [ ] 1.2 Kernel test: a terminal opened over a connection with no workspace root keeps an on-demand server past its idle timeout once the connection goes away without `term/close`, and an exited-while-detached terminal is still listed (the idle rule already counts terminals; this pins it — no server change expected)
- [ ] 1.3 Fake-ssh test: run a long-lived command in a remote project's terminal, `CloseProject`, and assert the server still lists the terminal with zero clients (fails today: `term/close` is sent before the disconnect)
- [ ] 1.4 Fake-ssh test: the same through `Shutdown` (quit), asserting the shell is still running on the host (passes today — quit orphans; pins that the identity written by § 4 refers to a live terminal)

## 2. Pending host panes (deferred start)

- [ ] 2.1 `HostTerminalSource::Ready()`; `TerminalSession`: pending state carrying `PendingHostStart{cwd, command, identity}`, `SetPendingStatus(text)`, input dropped while pending; one `AppendStatusLineLocked` replacing the `StartOnHost`-failure, `HostConnectionLost` and `MakeGapLine` builders
- [ ] 2.2 `make_started_pane`: return a pending pane when the launcher is a host source that is not `Ready`, instead of null
- [ ] 2.3 `TerminalPanelService::StartPendingPanes(launcher)`; `RemoteHostService` calls it from the project's first `Ready` (`project_reconnected`) and the host's `Ready`; `ActivateProjectState` calls it for a project whose launcher is already `Ready`
- [ ] 2.4 `OpenTerminalOnHost` opens its pending pane at once; delete `Host::pending_terminals` and `OpenPendingTerminals`; fold `OpenTerminal`/`OpenTerminalOn` onto one push-tab helper
- [ ] 2.5 On `Disconnected`/`Offline`, pending panes show the error line and relaunch with Enter (existing `relaunch_pane`)
- [ ] 2.6 Make 1.1 pass; panel-service unit tests (placeholder mode) for the `+` during `Connecting`, for input dropped while pending, and for the status line rewritten only on a state change

## 3. Explicit detach on close-project; close on opt-out

- [ ] 3.1 `TerminalHostChannel::Detach()` / `RemoteTerminalChannel::Detach()`: unregister and stop frames, no `term/close`
- [ ] 3.2 `TerminalSession::DetachFromHost()` retaining the identity; destruction of an undetached host session still closes
- [ ] 3.3 `TerminalPanelService::DetachHostTerminals()` and `CloseHostTerminals()` over every host pane (project and Open Terminal on Host); detach only a running one — a pane whose shell already exited is closed by both (its buffer would otherwise count as live for the server's idle rule)
- [ ] 3.4 `WorkspaceShell::CloseProject`: detach (or close with `terminal.restore_sessions` off) before `ProjectCatalogService::Close`; `LifecycleCoordinator::Shutdown`: close host terminals with it off, before `persist_active_project`
- [ ] 3.5 Make 1.3 pass; add "closing the tab still ends the shell", "restore_sessions off closes on quit and on close-project", and "three shells ended with Ctrl+D, tabs left open, quit: the server lists no terminal and idles out" tests

## 4. Persist host identity

- [ ] 4.1 `HostIdentity` on `TerminalHostChannel::identity()` and `TerminalSession::host_identity()` (live channel, else retained; cleared by `Stop`)
- [ ] 4.2 `TerminalPaneRecord`: `host_epoch`, `host_handle`, `host_target`; rewrite the header comment (host panes' processes ARE kept)
- [ ] 4.3 New `TerminalPaneTag`s in `WorkspacePersistenceBinaryFormatSessions.cpp` with decode bounds (epoch ≤ `kMaxEpochBytes`, handle 0 = none)
- [ ] 4.4 `CaptureLayout`: record host panes, including launcher-bound ones, with their identity and target
- [ ] 4.5 Round-trip test; a session without the tags revives as a fresh shell; corpus entries for the session fuzz target, and run it

## 5. Reattach on reopen

- [ ] 5.1 Revive: a record with an identity becomes a pending pane carrying it; a `host_target` record resolves its launcher through `RemoteHostService::Ensure` after `ParseRemoteHostTarget`
- [ ] 5.2 `StartPendingPanes`: cold `RemoteTerminalChannel::Attach` on a matching epoch; on mismatch or a refused attach, `Open` in the recorded cwd; count the reason per host
- [ ] 5.3 The dim `⋯ reattached ⋯` rule under the restored history (D1 helper)
- [ ] 5.4 Reattached exited shell: final output and exit line shown, Enter relaunches in the same directory (test)
- [ ] 5.5 Fake-ssh end to end: quit, reopen, same handle; screen content printed after the quit present and pre-quit lines in scrollback; plus the epoch-changed fallback (restart the fake host's server between the two)

## 6. Discover unclaimed host terminals

- [ ] 6.1 Server: the workspace root recorded on the `Terminal` at open; `TerminalTable::List` (bounded by `max_terminals`); `term/list` in `RemoteProtocol.h` and `RemoteServer.cpp`; kernel test covering an attached, a detached and an exited terminal across two roots
- [ ] 6.2 Client: send after the session's attaches; filter (no clients, this root or a terminal-only host, not just claimed); `kErrorUnknownMethod` = empty list
- [ ] 6.3 The per-host summary row (reattached · started fresh with reason · running with no tab) with `RemoteAttachHostTerminals` / `RemoteCloseHostTerminals`; `Remote: Show Status` appends the unclaimed terminals
- [ ] 6.4 Tests: a crash-orphaned terminal is offered and Attach opens it with its screen; one attached by another client is not offered; Close ends it

## 7. QA, docs and lanes

- [ ] 7.1 `qa-remote`: the scenarios of § 8, as a new lettered group `T` in `tools/qa/remote.py` (the relaunch ones reuse `L`'s second launch with the same XDG dirs); every `T` scenario also runs under `qa-slow`
- [ ] 7.2 Verify by hand on `worky` with `watch date`: T1, T2, T3 and T6
- [ ] 7.3 Docs: `remote-projects.md` § 6.12/§ 7.4a as built, `active-work.md` § 3 (host terminals survive a quit), `known-tech-debt.md`: the no-project host pane (D3) and anything else deferred
- [ ] 7.4 Lanes, tests run in batches of 16: `tools/run-checks.sh tests`, `perf-tests` (a render-adjacent session change), `qa`, `qa-remote`, `asan`, `ubsan`, `tsan`, `clang-build`; `tools/audit-lua-longjmp.py` is not affected

## 8. QA scenarios (`qa-remote`, group `T`)

Each runs the app against the loopback sshd (`tools/qa/local-sshd.sh`) or `worky`,
and checks the HOST side through `ssh host microide-server status` (terminal
count) and `pgrep -f` of the marker command, not only what the app shows. The
on-demand server is started with a short `--idle-timeout-ms` so the idle-out
assertions take seconds. Each scenario records a screenshot of the panel.

- [ ] T1 **Three shells survive a quit.** Open the remote project; three tabs: `watch -n1 date` (long), `ls -la` and `ls -lh` (output), the third split in two panes. Quit; relaunch with the same XDG dirs. Assert three tabs in the same order, the split kept, the first showing a `date` newer than the quit, the `ls` outputs in the other two's scrollback, every cwd as before, and `status` reporting 3 terminals before and after
- [ ] T2 **Closed with X, nothing remains.** From T1's state, close the three tabs with the X; quit. Assert `pgrep watch` empty, `status` 0 terminals, and the on-demand server idles out
- [ ] T3 **Ended with Ctrl+D, tabs left open, nothing remains.** From T1's state, Ctrl+D in each (the `watch` one after Ctrl+C), leave the `[process exited]` tabs open; quit. Same assertions as T2; on relaunch the three tabs revive as fresh shells in their directories
- [ ] T4 **The host server restarted in between.** From T1's state, quit; `microide-server stop` then `start` on the host; relaunch. Assert three fresh shells in the recorded directories and one row naming one host, three started fresh, "the host server restarted"
- [ ] T5 **A command finished while microide was closed.** A tab runs `sleep 3; echo DONE-<token>; exit`; quit at once; wait; relaunch. Assert the tab shows `DONE-<token>` and the exit line, and Enter starts a new shell in that directory
- [ ] T6 **A crash leaves an orphan that is found.** With `watch` running, SIGKILL the app (no session written for the new tab). Relaunch. Assert the row reports one terminal running with no tab; Attach opens a tab showing a live `date`; Close ends it (`pgrep` empty, `status` 0)
- [ ] T7 **Opted out.** `terminal.restore_sessions` off; with `watch` running, quit. Assert `pgrep` empty and `status` 0; relaunch shows one fresh default terminal
- [ ] T8 **Close-project keeps the shell; reopening reattaches.** With `watch` running, close the project tab (not the app). Assert `status` 1 terminal, 0 clients. Reopen the project from Recents: the tab reattaches with a newer `date`
- [ ] T9 **`+` while connecting.** Relaunch with the host behind the 60 ms link, click `+` before the status reads Ready. Assert a second tab appears at once reading as connecting and holds a host prompt once Ready, with no tab added or removed
- [ ] T10 **Open Terminal on Host, with and without a project.** (a) With the remote project active, `Remote: Open Terminal on Host…`, run `watch`, quit, relaunch: the host tab is back with its title prefix and a newer `date`. (b) With no project open, the same, quit, relaunch, open the project: the row offers the terminal as running with no tab and Attach opens it
- [ ] T11 **Deep scrollback after a reattach.** `seq 1 3000` in a tab; quit; relaunch. Assert the last lines present at once and `1` reachable by scrolling up (older pages fetched), with no duplicated line across the prefetch boundary
- [ ] T12 **A terminal another client uses is not offered.** Two app instances with different XDG dirs on the same project: the first runs `watch`; the second connects. Assert the second's row does not offer it; quit the first; reconnect the second (`Remote: Reconnect`): now it is offered
- [ ] T13 **Show Status lists the unclaimed.** From T6's orphan, before Attach, `Remote: Show Status` names the host with one unclaimed terminal and its cwd
- [ ] T14 **Closing a running tab ends its process.** With `watch` running, close the tab with X (app kept open). Assert `pgrep` empty within a second and `status` 0 terminals
- [ ] T15 **Every `T` scenario under `qa-slow`** (60 ms each way), with the reattach timings recorded against the three-second budget of the proposal's first scenario
