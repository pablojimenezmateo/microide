## Context

`RemoteHostService` serves two kinds of connection: a remote project's (`Project`,
`projects_`, `ApplyProject`) and a terminal-only host's (`Host`, `hosts_`,
`Apply`). The second exists for one command. Every other remote verb — reconnect,
disconnect, log, status, stop server, port forwards — resolves its session through
`SessionFor(host)`, "its terminal session, else a project's", so they work on
project sessions alone.

## Goals / Non-Goals

**Goals:** one kind of host connection in the service; no user-facing verb that
duplicates Terminal > New; the auth ssh tab unchanged.

**Non-Goals:** touching the server, the wire, or `TerminalPanelService::OpenTerminalOn`.

## Decisions

### D1. Delete, do not deprecate
The command is removed outright with its `ActionId`; the palette entry disappears,
and `remote-terminal` on the control channel answers "unknown command" as any
removed command does. Nothing persisted names it.

### D2. `SessionFor` becomes "the project session for this host"
With `hosts_` gone, `SessionFor(host)` is the first project whose record's host
matches, and `SoleHost` the sole host among projects. `Reconnect`/`Disconnect` keep
their project loops. `StatusText` keeps its project lines. `PortWatch` and
`forwards_` are already keyed by host and fed by project sessions.

### D3. The interactive-auth tab keeps `OpenTerminalOn`
`ApplyProject`'s `NeedsAuth` opens `open_terminal(LocalLauncher(), "ssh · ",
SshCommandText())`. That is the surviving user of `TerminalPaneState::launcher` and
`label_prefix`, and of `TerminalPanelService::OpenTerminalOn`; they stay, with
their comments repointed from "a terminal opened on a host" to "a pane that runs
through a launcher other than the project's (the ssh auth tab)".

### D4. `CaptureLayout` keeps skipping launcher-bound panes
A launcher-bound pane is now only the auth tab, which a session must not revive.
`remote-terminal-restore` therefore records host identity for project panes only
and needs no `host_target`.

## Risks / Trade-offs

- [A user relied on a persistent host shell with no project] → `Remote: Open
  Folder on Host…` on any directory gives the same shell with a project around it;
  `ssh` in a local terminal gives it without one.
- [Tests lose coverage of the status-bar host segment and `label_prefix`] → Moved
  to `TestOpenFolderOnHostEditsTheHostTree` (segment) and a project-based mixed
  terminals test (prefix on the auth tab).

## Migration Plan

Removal only; revert restores it.

## Open Questions

None.
