## Why

`Remote: Open Terminal on Host…` opens a shell on a host **without** a project
there: a terminal-only connection with its own `RemoteHostSession` and launcher.
It predates remote projects (Phase 2a, "a host's terminals with no project"). Since
Phase 2b a remote project's Terminal > New already runs on that project's host
(TD-2026-09-22-301), so the command's remaining value is a shell on a host where
the user has not opened a folder, or on a second host — what `ssh` in any local
terminal gives, with tmux for persistence.

For that it owns a parallel subsystem (verified 2026-10-27): `RemoteHostService::
Host`, the `hosts_` map, `Ensure` (whose only caller is `OpenTerminalOnHost`),
`OpenPendingTerminals` and its counter, the per-host `Apply` status switch (~90
lines, a near-copy of `ApplyProject`'s), a second interactive-auth terminal path,
and host loops in `Reconnect`, `Disconnect`, `StatusText` and `SoleHost`. Three
proposals in flight pay for it too: `remote-terminal-restore` needs a `host_target`
session field, a "pane with no project has no session to live in" tech-debt entry
and a QA scenario for it; `remote-menu` an item; `remote-host-activity` a second
kind of host entry. Removing the command deletes more code than those three add.

## What Changes

- **Remove** `ActionId::RemoteOpenTerminal` (`remote-terminal`), its palette and
  Terminal-menu entries, availability case and Help text.
- **Delete** the terminal-only host session: `RemoteHostService::Host`, `hosts_`,
  `Ensure`, `OpenTerminalOnHost`, `OpenPendingTerminals`, `Apply(host, status)`,
  `Find(host)`, the host branches of `SessionFor`, `Reconnect`, `Disconnect`,
  `StatusText`, `SoleHost`, `AdviseAboutHostServer`'s host caller, and the host
  variant of the NeedsAuth ssh tab. A "host" everywhere in the service is now a
  remote project's host.
- **Keep** the internal `open_terminal(launcher, prefix, command)` operation and
  `TerminalPanelService::OpenTerminalOn`: the interactive-auth ssh tab uses it with
  the local launcher, and `TerminalPaneState::launcher`/`label_prefix` stay for it.
- **Keep** the server's terminal-only hello (`root == ""`): nothing on the wire
  changes, and an older client may still connect that way.
- Tests: `TestOpenTerminalOnHostFromTheCommand` and `TestMixedTerminalsSayWhereTheyRun`
  are deleted; the behaviours they also covered (status-bar host segment, the
  host-prompt completion, `label_prefix` on a host tab) move to project-based
  tests or already have them.

## Capabilities

### Modified Capabilities

- `remote-connection-lifecycle`: the requirement "Host terminals open from a
  command" is removed, with its "host and local terminals are never confused"
  scenario restated for the surviving case (a remote project's host terminal beside
  a local project's local one).

## Impact

- `src/workspace/services/RemoteHostService.{h,cpp}`: the deletions above (≈ 250
  lines); `src/workspace/actions/*`, `registries/WorkspaceCommandRegistry.cpp`,
  `registries/WorkspaceMenuRegistry.cpp`, Help/About.
- `tests/RemoteHostServiceTests.cpp`: two tests deleted, assertions moved.
- Docs: `remote-projects.md` § 7's entry-point table and § 8's Phase 2a note (as
  built: removed, and why), `active-work.md`.
- Sequencing: first of the four remote changes; `remote-terminal-restore`,
  `remote-menu` and `remote-host-activity` are written against the tree after it.
