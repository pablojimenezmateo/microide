## 1. Remove the command

- [ ] 1.1 Delete `ActionId::RemoteOpenTerminal`, its `ActionSpec` (`remote-terminal`), the Terminal-menu item, its `WorkspaceActionAvailability` case, the executor case and any Help/About text naming it
- [ ] 1.2 Delete `RemoteHostService::OpenTerminalOnHost`, `Ensure`, `OpenPendingTerminals`, `Host`, `hosts_`, `Find(host)`, `Apply(host, status)` and the host variant of the NeedsAuth ssh tab; make `SessionFor`, `SoleHost`, `Reconnect`, `Disconnect`, `StatusText`, `AdviseAboutHostServer` and `WatchPorts` project-only
- [ ] 1.3 Repoint the comments on `TerminalPaneState::launcher`/`label_prefix`, `TerminalPanelService::OpenTerminalOn` and `RemoteHostService::Operations::open_terminal` to the auth tab

## 2. Tests

- [ ] 2.1 Delete `TestOpenTerminalOnHostFromTheCommand` and `TestMixedTerminalsSayWhereTheyRun`; move the status-bar host segment assertions to `TestOpenFolderOnHostEditsTheHostTree` and add a project-based "the auth tab is prefixed `ssh · ` and a project terminal is not" test
- [ ] 2.2 `tools/run-checks.sh tests` (batches of 16), `clang-build`, `qa`, `qa-remote`

## 3. Docs

- [ ] 3.1 `remote-projects.md` § 7 entry-point table and § 8 Phase 2a note (removed, why); `active-work.md` § remote; `known-tech-debt.md` if anything is deferred
