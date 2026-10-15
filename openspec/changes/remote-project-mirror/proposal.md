## Why

Phases 2b and 3 of the remote-projects design shipped a remote PROJECT (a host
folder opened on a local mirror, saves pushed under compare-and-swap, conflicts,
held deletes, Changed on Host) but were archived without specs: `openspec/specs/`
holds the server, protocol, lifecycle, spawn and terminal contracts and nothing
about the mirror the user actually edits in. The 2026-10-16 verification on a real
host (`worky`, headless, every scenario driven as a person would) found the shipped
behaviour sound and two gaps that stopped an agent or a driver cold: the
changed-on-disk banner was mouse-only, so a host edit under a dirty tab left every
later save refused with no way to answer it from the control channel; and the
host entry points were reachable only from the command palette. Both are fixed on
`main`; this change records the contract they belong to and the remainder the run
left: a mirror cannot be removed from inside the app, and the control channel
reports no remote state, so a driver infers readiness from a notification row.

## What Changes

- A durable `remote-project-mirror` spec: what the mirror is equal to and when, how a
  save reaches the host, what the user sees when the host changes a file under a
  clean tab, a dirty tab, and a saved-but-unpushed file, what a dead link and an
  offline save do, when a mass delete is held, what reopening the mirror does.
  Every requirement is stated as it was verified on the host (and as the unit,
  parity and host-service suites pin it).
- `remote-connection-lifecycle`: the host entry points sit beside their local twins
  (File menu, Terminal menu, welcome page), not only in the palette (shipped
  57908e83).
- `external-repo-change-handling`: the editor banner's buttons are pressable from
  the control channel (`banner-action`, shipped 61b096f2).
- **New**: `Remote: Remove Mirror` deletes a closed remote project's mirror (tree,
  objects, journal, state) after the dirty prompt, and the recents entry with it.
  A mirror of this repository is 47 MB of tree plus up to 170 MB of metadata after
  heavy churn, and today it leaves only by hand under `~/.local/share/microide/remote`.
- **New**: the `status` and `projects` control queries report a remote project's
  connection state, sync progress and pending-push count, so a driver or agent
  reads readiness instead of inferring it from a notification's wording.
- The host scenario suite used for the verification lands as `tools/qa/remote.py`
  on the `tools/qa` driver (the `gui-qa-harness` change's task 5.2), so the next
  pass is one command.

## Capabilities

### New Capabilities
- `remote-project-mirror`: the remote project's user-visible contract — mirror
  equality, save and push, host changes under open tabs, conflicts, link loss and
  offline saves, held deletes, reopening, mirror removal, control-channel state.

### Modified Capabilities
- `remote-connection-lifecycle`: adds the requirement that the host entry points
  are in the File and Terminal menus and on the welcome page.
- `external-repo-change-handling`: adds the requirement that the changed-on-disk
  banner's actions are reachable from the control channel.
- `remote-process-spawn`: a kept process (language server, debug adapter) nobody
  reattaches to within the server's keep grace is terminated and forgotten. The
  verification found four clangd processes with a background index on the host
  after four short client sessions, one per session, each kept for the server's
  lifetime.

## Impact

- `src/workspace/services/RemoteHostService*` (Remove Mirror; state for the
  queries), `src/project/remote/RemoteProject` / `MirrorStore` (delete), 
  `src/workspace/control/ControlChannelService.cpp` (`status` / `projects`
  fields), `src/workspace/registries/WorkspaceCommandRegistry.cpp` (one verb),
  `src/workspace/persistence/RecentsService` (drop the entry).
- `tools/qa/remote.py` (new; coordinated with the `gui-qa-harness` owner — it
  adds a file and edits nothing of the driver).
- Docs: `dev-docs/control/control-channel.md` (query fields), README command list,
  `docs/microide.1` (regenerated), `dev-docs/design/remote-projects.md` § 7.1 and
  § 6.8 pointing at the spec.
- Server: `ProcessTable` keep grace and the `--keep-grace-ms` option (no wire change).
