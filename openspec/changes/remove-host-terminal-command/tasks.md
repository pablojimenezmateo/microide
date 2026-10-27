## 1. Remove the command

- [x] 1.1 Delete `ActionId::RemoteOpenTerminal`, its `ActionSpec` (`remote-terminal`), the Terminal-menu item, its `WorkspaceActionAvailability` case, the executor case, the `remote-terminal` completion branch in `WorkspaceCommandLineCoordinator.cpp`, the host-prompt dispatch to it in `WorkspaceShellPrompts.cpp` (and `PromptSurfaceState::Action::RemoteHost` if nothing else opens it; `OpenRemoteHostPrompt` loses `bool folder`), and any Help/About text naming it
- [x] 1.2 Delete `RemoteHostService::OpenTerminalOnHost`, `Ensure`, `OpenPendingTerminals`, `Host`, `hosts_`, `Find(host)`, `has_hosts()`, `Apply(host, status)` and the host variant of the NeedsAuth ssh tab; drop the host loop from `SessionFor`, `SoleHost`, `KnownHosts`, `Reconnect`, `Disconnect`, `StatusText` (and its "or Remote: Open Terminal on Host…" sentence), `OpenHostLocalUrl`, `PublishStatusSegment` and the destructor; make `AdviseAboutHostServer` and `WatchPorts` project-only
- [x] 1.3 Repoint the comments on `TerminalPaneState::launcher`/`label_prefix`, `TerminalPanelService::OpenTerminalOn` and `RemoteHostService::Operations::open_terminal` to "a launcher other than the project's (the auth tab, a local terminal in a remote project)"

## 2. The local terminal

- [x] 2.1 `ActionId::RemoteOpenLocalTerminal`, `ActionSpec{"remote-local-terminal", "Remote: Open Local Terminal"}`, Terminal-menu item `New Local Terminal` under New Terminal, availability = the active project is remote, executor → `RemoteHostService::OpenLocalTerminal()` → `open_terminal(LocalLauncher(), "local · ", {})`
- [x] 2.2 Test: in a remote project the command opens a tab prefixed `local · ` whose session runs through the local launcher; in a local project the action is unavailable and the menu item dims

## 3. Tests

- [x] 3.1 Delete `TestOpenTerminalOnHostFromTheCommand` and `TestMixedTerminalsSayWhereTheyRun`; move the status-bar host segment assertions to `TestOpenFolderOnHostEditsTheHostTree`; add a project-based "the auth tab is prefixed `ssh · `, the local terminal `local · `, and a project terminal neither" test
- [x] 3.2 `tools/qa/remote.py` scenario `G`: open the terminal with `term` in the remote project instead of `remote-terminal`, keep the `hostname` assertion
- [ ] 3.3 `tools/run-checks.sh tests` (batches of 16), `clang-build`, `qa`, `qa-remote`

## 4. Docs

- [x] 4.1 `remote-projects.md` § 7.1 entry-point table (Terminal-menu host entry removed; `Open Local Terminal` built) and § 8 Phase 2a note (removed, why); `active-work.md` § remote; `known-tech-debt.md` if anything is deferred
