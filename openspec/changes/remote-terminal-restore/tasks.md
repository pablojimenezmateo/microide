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

- [ ] 7.1 `qa-remote`: extend scenario `L` (the relaunch with the same XDG dirs) with a one-second ticking command before the quit; after the relaunch assert the tab shows a newer tick and the earlier ones in scrollback (also under `qa-slow`)
- [ ] 7.2 Verify by hand on `worky` with `watch date`
- [ ] 7.3 Docs: `remote-projects.md` § 6.12/§ 7.4a as built, `active-work.md` § 3 (host terminals survive a quit), `known-tech-debt.md`: the no-project host pane (D3) and anything else deferred
- [ ] 7.4 Lanes, tests run in batches of 16: `tools/run-checks.sh tests`, `perf-tests` (a render-adjacent session change), `qa`, `qa-remote`, `asan`, `ubsan`, `tsan`, `clang-build`; `tools/audit-lua-longjmp.py` is not affected
