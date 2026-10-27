## Why

The remote capabilities are fourteen `Remote: …` commands (verified 2026-10-27 in
`WorkspaceCommandRegistry.cpp`, after `remove-host-terminal-command`), of which
exactly one is in a menu: `Remote: Open Folder on Host…` under File, placed there
by `remote-connection-lifecycle`'s "the host entry points sit beside their local
twins". The other thirteen — status, log, Changed on Host, port forwarding,
reconnect, disconnect, stop server, remove mirror, and the rest — are palette-only,
so a user who has not learned the `Remote:` prefix cannot find them, and the one
that is in a menu carries a category prefix inside a menu that already is a
category ("File > Remote: Open Folder on Host…"). The status bar's remote segment,
which names the connected host, has no click action at all
(`StatusBarSegmentValue::command` is empty for it).

Two proposals in flight add more: `remote-terminal-restore` adds Attach/Close for
unclaimed terminals, and `remote-host-activity` turns `Show Status` into a Host
view and adds End Terminal/Process. Scattering those too would make it worse.

## What Changes

- **A top-level `Remote` menu**, after Git and before Terminal, holding every
  user-facing remote verb with its prefix dropped: Open Folder on Host…, Show
  Status, Show Changed on Host, Show Host Server Log, Forward a Port…, Stop
  Forwarding a Port…, Reconnect, Disconnect, Stop Host Server, Remove Mirror….
  Commands that need an argument the menu cannot supply (Resolve Conflict
  `<path>`, Copy Command `<kind>`, Clear Changed on Host, Apply Held Deletes) stay
  palette- and notification-driven.
- **The twin leaves File.** The welcome page's Open Folder on Host… button stays
  (it is not a menu, and it is the cold-start path).
- **The palette keeps the `Remote:` prefix.** In the palette the prefix *is* the
  category, as VS Code's `Remote:` and `Git:` prefixes are; typing "remote" must
  still list them. Only menu labels drop it, through the existing per-item label
  override (`MenuItem(ActionId, "label")`).
- **The status bar's remote segment opens the Remote menu**, as a popup anchored
  at the segment — VS Code's remote indicator, built from the menu popup the
  overflow chevron and the Compact-mode hamburger already use. One entry point for
  "what can I do with this host", reached from the bar that shows the host.
- Items whose action is not available (no host connected) follow the menu's
  existing availability rule, so the menu never offers a verb with nothing to act
  on.

Out of scope: a Remote submenu per host (one host is the common case and the
commands prompt when there are several), moving the Changed on Host and Host
sidebar views (they are views, reached from this menu), and any change to the
commands themselves.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `remote-connection-lifecycle`: "The host entry points sit beside their local
  twins" becomes "The remote verbs live in a Remote menu": the File and Terminal
  placements are removed, the welcome button is kept, and the status segment's
  click is specified.

## Impact

- `src/workspace/registries/WorkspaceMenuRegistry.*`: `MenuId::Remote`,
  `kRemoteItems`, the File removal, menu-label overrides.
- `src/workspace/services/RemoteHostService.cpp` (`PublishStatusSegment`) and the
  status-bar click routing: the segment's command opens the Remote popup.
- `src/workspace/coordinators/WorkspaceChromeMouseCoordinator.cpp`: the popup
  anchored at a status segment (today popups anchor at menu-bar items and the
  chevron).
- `src/workspace/actions/WorkspaceActionAvailability.cpp`: every menu-present
  remote id has an availability answer.
- Tests: menu registry (the Remote menu's items and order, File/Terminal without
  the twins, labels without the prefix, palette labels with it), layout (the menu
  bar still fits at the narrowest Normal width, else overflows with the chevron,
  never truncates), the segment click opening the popup, and a `qa` scenario
  walking the Remote menu by keyboard. The registry test reads "File without the
  twin".
- Docs: `remote-projects.md` § 7's entry-point table, the comment in
  `WorkspaceChromeMouseCoordinator.cpp` that names "File > Remote: Open Folder on
  Host", Help/About if it lists menus.
- Sequencing: lands before `remote-host-activity` (whose Show Status and End
  verbs then slot into this menu) and is independent of
  `remote-terminal-restore`.
