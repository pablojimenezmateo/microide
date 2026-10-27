## Why

`Remote: Open Terminal on Host…` opens a shell on a host **without** a project
there: a terminal-only connection with its own `RemoteHostSession` and launcher.
It predates remote projects (Phase 2a, "a host's terminals with no project"). Since
Phase 2b a remote project's Terminal > New already runs on that project's host
(TD-2026-09-22-301), so the command's remaining value is a shell on a host where
the user has not opened a folder, or on a second host — what `ssh` in any local
terminal gives, with tmux for persistence.

For that it owns a parallel subsystem (verified 2026-10-27 against
`RemoteHostService.{h,cpp}`): `RemoteHostService::Host`, the `hosts_` map,
`Ensure` (whose only caller is `OpenTerminalOnHost`), `OpenPendingTerminals` and
its counter, the per-host `Apply` status switch (~80 lines, a near-copy of
`ApplyProject`'s), a second interactive-auth terminal path, and a host loop in
every per-host query — `SessionFor`, `SoleHost`, `KnownHosts`, `Reconnect`,
`Disconnect`, `StatusText`, `OpenHostLocalUrl`, `PublishStatusSegment` and the
destructor — plus `has_hosts()`, which nothing calls. Three proposals in flight
pay for it too: `remote-terminal-restore` would need a `host_target` session field,
a "pane with no project has no session to live in" tech-debt entry and a QA
scenario for it; `remote-menu` an item; `remote-host-activity` a second kind of
host entry. Removing the command deletes more code than those three add.

One thing the command did is not covered by "ssh in a local terminal": a user whose
only open project is remote has no local terminal to type `ssh` into (Terminal >
New runs on the host there). Design § 7.1 already lists `Remote: Open Local
Terminal` for exactly that and it was never built. It is one `ActionId` over the
kept `open_terminal(LocalLauncher(), …)` operation, so this change adds it as the
migration rather than leaving a hole.

## What Changes

- **Remove** `ActionId::RemoteOpenTerminal` (`remote-terminal`): its `ActionSpec`,
  Terminal-menu item, availability case, executor case, Help text, the
  `remote-terminal` branch of the command-line completer
  (`WorkspaceCommandLineCoordinator.cpp`), and the host-prompt completion that
  picks it (`WorkspaceShellPrompts.cpp`, `PromptSurfaceState::Action::RemoteHost`
  → `RemoteOpenTerminal`). `Action::RemoteHost` itself stays only if another
  prompt still uses it; `OpenRemoteHostPrompt(bool folder)` loses its `folder`
  argument either way.
- **Delete** the terminal-only host session: `RemoteHostService::Host`, `hosts_`,
  `Ensure`, `OpenTerminalOnHost`, `OpenPendingTerminals`, `Apply(host, status)`,
  `Find(host)`, `has_hosts()`, and the host branch of `SessionFor`, `SoleHost`,
  `KnownHosts`, `Reconnect`, `Disconnect`, `StatusText` (and its "or Remote: Open
  Terminal on Host…" sentence), `OpenHostLocalUrl`, `PublishStatusSegment` and
  the destructor; `AdviseAboutHostServer`'s host caller; the host variant of the
  NeedsAuth ssh tab. A "host" everywhere in the service is now a remote project's
  host.
- **Add `Remote: Open Local Terminal`** (`remote-local-terminal`, menu label
  `New Local Terminal` under Terminal > New Terminal): a terminal tab through the
  local launcher, prefixed `local · `, available only when the active project is
  remote (in a local project Terminal > New is already local, so the item is
  dimmed there). It is the production caller that justifies keeping
  `TerminalPanelService::OpenTerminalOn` beside the auth tab.
- **Keep** the internal `open_terminal(launcher, prefix, command)` operation,
  `TerminalPanelService::OpenTerminalOn`, and `TerminalPaneState::launcher` /
  `label_prefix`: the interactive-auth ssh tab and the local terminal above use
  them.
- **Keep** the server's terminal-only hello (`root == ""`): nothing on the wire
  changes, and an older client may still connect that way.
- Tests: `TestOpenTerminalOnHostFromTheCommand` and `TestMixedTerminalsSayWhereTheyRun`
  are deleted; the behaviours they also covered (status-bar host segment, the
  host-prompt completion, `label_prefix` on a launcher-bound tab) move to
  project-based tests, one of which covers the new local terminal.
- `tools/qa/remote.py` scenario `G` (a host terminal through `remote-terminal`)
  becomes a project terminal through `term` in the remote project, asserting the
  host's `hostname` as it does today.

## Capabilities

### Modified Capabilities

- `remote-connection-lifecycle`: the requirement "Host terminals open from a
  command" is removed; "The host entry points sit beside their local twins" gains
  the local terminal in a remote project and restates "host and local terminals
  are never confused" for the surviving cases (a remote project's host terminal,
  its local terminal, and a local project's terminal).

## Impact

- `src/workspace/services/RemoteHostService.{h,cpp}`: the deletions above (≈ 250
  lines) and `OpenLocalTerminal()` (≈ 10 lines over `open_terminal`).
- `src/workspace/actions/WorkspaceActionTypes.h`, `WorkspaceActionAvailability.cpp`,
  `WorkspaceGlobalActionExecutor.cpp`, `WorkspaceActionServicesContextFactory.cpp`
  (`OpenRemoteHostPrompt`), `registries/WorkspaceCommandRegistry.cpp`,
  `registries/WorkspaceMenuRegistry.cpp` (`kTerminalItems`),
  `shell/WorkspaceShellPrompts.cpp`, `coordinators/WorkspaceCommandLineCoordinator.cpp`,
  Help/About.
- `tests/RemoteHostServiceTests.cpp`: two tests deleted, assertions moved, one
  added; `tools/qa/remote.py` scenario `G`.
- Docs: `remote-projects.md` § 7.1's entry-point table (the Terminal-menu entry
  removed, `Open Local Terminal` built) and § 8's Phase 2a note (as built:
  removed, and why), `active-work.md`.
- Sequencing: first of the four remote changes; `remote-terminal-restore`,
  `remote-menu` and `remote-host-activity` are written against the tree after it.
