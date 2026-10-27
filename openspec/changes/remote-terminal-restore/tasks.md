## 1. Pin the current behaviour with failing tests

- [ ] 1.1 Fake-ssh end-to-end test: open a remote project whose link becomes `Ready` after a delay; assert a terminal tab exists from project open (fails today: the pane is dropped)
- [ ] 1.2 Kernel test: a terminal opened over a connection with no workspace root keeps an on-demand server alive after the connection goes away without `term/close`; fix the server's exit rule if it does not
- [ ] 1.3 Fake-ssh test: run a long-lived command in a remote project's terminal, destroy the `WorkspaceShell` (quit) and assert the server still lists the terminal with zero clients (records whether today's order kills or orphans it)
- [ ] 1.4 Same as 1.3 for `CloseProject` (fails today: `term/close` is sent before the disconnect)

## 2. Pending host panes (deferred start)

- [ ] 2.1 `TerminalSession`: an unstarted pending state carrying `PendingHostStart{cwd, command, epoch, handle}`; input dropped while pending
- [ ] 2.2 `make_started_pane`: return a pending pane when the launcher is remote and its connection is not `Ready`, instead of null
- [ ] 2.3 `RenderViewModelBuilder`: precompose the pending pane's one-line status from the project or host connection state (no string building in the render TU)
- [ ] 2.4 Start pending panes on `Ready`: from `project_reconnected` for the project's root, and from the host's `Ready` replacing the `pending_terminals` counter in `OpenPendingTerminals`
- [ ] 2.5 On `Disconnect`, pending panes show it and relaunch with Enter (reuse `relaunch_pane`)
- [ ] 2.6 Make 1.1 pass; add a panel-service unit test (placeholder mode) for the `+` during `Connecting`

## 3. Explicit detach on quit and close-project

- [ ] 3.1 `TerminalHostChannel::Detach()` and `RemoteTerminalChannel::Detach()`: unregister and stop frames, with no `term/close`
- [ ] 3.2 `TerminalSession::DetachFromHost()`; destruction of an undetached host session still closes
- [ ] 3.3 `TerminalPanelService::DetachHostTerminals()` over every host pane (project and Open Terminal on Host)
- [ ] 3.4 Call it from quit (before the session is captured and flushed) and from `WorkspaceShell::CloseProject` (before `ProjectCatalogService::Close`), skipped when `terminal.restore_sessions` is off
- [ ] 3.5 Make 1.3 and 1.4 pass; add the "closing the tab still ends the shell" and "restore_sessions off closes on quit" tests

## 4. Persist host identity

- [ ] 4.1 `TerminalPaneRecord`: `host_epoch`, `host_handle`, `host_target`; update the header comment (processes ARE kept for host panes)
- [ ] 4.2 New `TerminalPaneTag`s in `WorkspacePersistenceBinaryFormatSessions.cpp` with decode bounds (epoch length cap, handle 0 = none)
- [ ] 4.3 `CaptureLayout`: record host panes, including launcher-bound (Open Terminal on Host) ones, with their identity
- [ ] 4.4 Round-trip test; an old session without the tags revives as a fresh shell; add corpus entries to the session fuzz target and run it

## 5. Reattach on reopen

- [ ] 5.1 Revive: a host record becomes a pending pane with `{epoch, handle}`; a `host_target` record resolves its launcher through `RemoteHostService::Ensure` (validated by `ParseRemoteHostTarget`)
- [ ] 5.2 On `Ready`: cold `RemoteTerminalChannel::Attach` when the epoch matches; on mismatch or a refused attach, fall back to `Open` in the recorded cwd
- [ ] 5.3 One notification row per host counting fallbacks with the reason (server restarted / no longer running / host session policy)
- [ ] 5.4 Reattached exited shell: shows its final output and exit line, and Enter relaunches in the same directory
- [ ] 5.5 Fake-ssh end to end: quit, then reopen, then the same handle, with screen content printed after the quit present and pre-quit lines in scrollback; plus the epoch-changed fallback test
- [ ] 5.6 Resolve the open question (a "reattached" rule in the tab) and implement the decision

## 6. Discover unclaimed host terminals

- [ ] 6.1 `RemoteProtocol.h` `term/list`, a validating decoder, and `TerminalTable::List` (capped) answered in `RemoteServer.cpp`; kernel test including exited and attached terminals
- [ ] 6.2 Client: send after the session's attaches; filter (no clients, this root or a terminal-only host, not just claimed); an unknown-method error means an empty list
- [ ] 6.3 Notification row with Attach (new tab per terminal, cold attach) and Close (`term/close`) as `ActionId`s; `Remote: Show Status` lists them
- [ ] 6.4 Tests: a crash-orphaned terminal is offered; one attached by another client is not

## 7. QA, docs and lanes

- [ ] 7.1 `qa-remote` scenario: a terminal running a one-second ticking command; quit; reopen; assert the tab shows a newer tick and the earlier ones in scrollback (also under the 60 ms link)
- [ ] 7.2 Verify by hand on `worky` with `watch date`
- [ ] 7.3 Docs: `remote-projects.md` § 6.12/§ 7.4a as built, `active-work.md` § 3 (process persistence for host terminals is shipped), `known-tech-debt.md` if anything is deferred
- [ ] 7.4 Lanes: `tools/run-checks.sh tests`, `qa`, `qa-remote`, `asan`, `ubsan`, `tsan` (batched at the end), and `clang-build`
