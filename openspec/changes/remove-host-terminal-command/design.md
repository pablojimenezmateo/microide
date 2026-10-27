## Context

`RemoteHostService` serves two kinds of connection: a remote project's (`Project`,
`projects_`, `ApplyProject`) and a terminal-only host's (`Host`, `hosts_`,
`Apply`). The second exists for one command. Every other remote verb — reconnect,
disconnect, log, status, stop server, port forwards — resolves its session through
`SessionFor(host)`, "its terminal session, else a project's", so they work on
project sessions alone. Verified 2026-10-27: `Ensure` has one caller
(`OpenTerminalOnHost`), `has_hosts()` has none, and every host loop in the service
is followed by the same loop over `projects_`.

## Goals / Non-Goals

**Goals:** one kind of host connection in the service; no user-facing verb that
duplicates Terminal > New; the auth ssh tab unchanged; a local shell reachable
from a remote-only window.

**Non-Goals:** touching the server, the wire, or
`TerminalPanelService::OpenTerminalOn`'s shape.

## Decisions

### D1. Delete, do not deprecate
The command is removed outright with its `ActionId`; the palette entry disappears,
and `remote-terminal` on the control channel answers "unknown command" as any
removed command does. Nothing persisted names it.

### D2. `SessionFor` becomes "the project session for this host"
With `hosts_` gone, `SessionFor(host)` is the first project whose record's host
matches, and `SoleHost` the sole host among projects. `Reconnect`/`Disconnect`,
`KnownHosts`, `StatusText`, `OpenHostLocalUrl` and `PublishStatusSegment` keep
their project loops and lose their host loops. `PortWatch` and `forwards_` are
already keyed by host and fed by project sessions.

### D3. The interactive-auth tab and the local terminal keep `OpenTerminalOn`
`ApplyProject`'s `NeedsAuth` opens `open_terminal(LocalLauncher(), "ssh · ",
SshCommandText())`. `Remote: Open Local Terminal` opens
`open_terminal(LocalLauncher(), "local · ", {})` from the same operation. Those
two are the users of `TerminalPaneState::launcher`, `label_prefix` and
`TerminalPanelService::OpenTerminalOn`; the comments are repointed from "a
terminal opened on a host" to "a pane that runs through a launcher other than the
project's (the ssh auth tab, a local terminal in a remote project)".

### D4. `CaptureLayout` keeps skipping launcher-bound panes
A launcher-bound pane is now the auth tab or a local terminal in a remote project.
Neither is revived by the session: the auth tab is a one-shot, and a local shell
in a remote project is a convenience the user re-opens (local revive is
layout-only anyway). `remote-terminal-restore` therefore records host identity
for project-launcher panes only and needs no `host_target`.

### D5. The local terminal is one `ActionId`, not a second launcher path
`ActionId::RemoteOpenLocalTerminal` → `RemoteHostService::OpenLocalTerminal()` →
`operations_.open_terminal(LocalLauncher(), "local · ", {})`. Availability: the
active project is remote (`ProjectId::remote()`), else "not now", so the menu item
dims in a local project instead of duplicating Terminal > New. The prompt-less
shape means no completion, no host argument and no new prompt action.

## Risks / Trade-offs

- [A user relied on a persistent host shell with no project] → `Remote: Open
  Folder on Host…` on any directory gives the same shell with a project around it;
  `ssh` in a local terminal (Terminal > New in a local project, or the new local
  terminal in a remote one) gives it without.
- [Tests lose coverage of the status-bar host segment and `label_prefix`] → Moved
  to `TestOpenFolderOnHostEditsTheHostTree` (segment) and a project-based mixed
  terminals test (prefix on the auth tab and on the local terminal; none on a
  project terminal).

## Migration Plan

Removal plus one additive command; revert restores the old command.

## Open Questions

None.
