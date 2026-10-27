## Why

The remote capabilities are fourteen `Remote: …` commands (verified 2026-10-27 in
`WorkspaceCommandRegistry.cpp`; fifteen today, fourteen after
`remove-host-terminal-command`, which also adds a Terminal-menu `New Local
Terminal` that is a terminal verb, not a remote one), of which exactly one is in a
menu: `Remote: Open Folder on Host…` under File, placed there by
`remote-connection-lifecycle`'s "the host entry points sit beside their local
twins". The other thirteen — status, log, Changed on Host, port forwarding,
reconnect, disconnect, stop server, remove mirror, and the rest — are palette-only,
so a user who has not learned the `Remote:` prefix cannot find them, and the one
that is in a menu carries a category prefix inside a menu that already is a
category ("File > Remote: Open Folder on Host…"). The status bar's remote segment,
which names the connected host, runs `remote-status` on click
(`PublishStatusSegment` sets `command = "remote-status"`, tooltip "Remote: Show
Status"): one verb hard-wired into the bar, and the design (§ 7.4) wanted the
click to list the remote verbs instead.

Two proposals in flight add more: `remote-terminal-restore` adds Attach/Close for
unclaimed terminals, and `remote-host-activity` turns `Show Status` into a Host
view and adds End Process. Scattering those too would make it worse.

## What Changes

- **A top-level `Remote` menu**, after Git and before Terminal, holding every
  user-facing remote verb with its prefix dropped: Open Folder on Host…, Show
  Status, Show Changed on Host, Show Host Server Log, Forward a Port…, Stop
  Forwarding a Port…, Reconnect, Disconnect, Stop Host Server, Remove Mirror….
  Commands that need an argument the menu cannot supply (Resolve Conflict
  `<path>`, Copy Command `<kind>`, Clear Changed on Host, Apply Held Deletes) stay
  palette- and notification-driven.
- **The twin leaves File.** The welcome page's Open Folder on Host… button stays
  (it is not a menu, and it is the cold-start path). Terminal keeps `New Local
  Terminal` (a terminal verb).
- **The palette keeps the `Remote:` prefix.** In the palette the prefix *is* the
  category, as VS Code's `Remote:` and `Git:` prefixes are; typing "remote" must
  still list them. Only menu labels drop it, through the existing per-item label
  override (`MenuItem(ActionId, "label")`).
- **The status bar's remote segment opens the Remote menu**, as a popup anchored
  at the segment — VS Code's remote indicator — instead of running `remote-status`.
  A segment that opens a menu says so with a `MenuId` on `StatusBarSegmentValue`;
  the click path, which already has the segment's rect, opens it through the
  anchored-menu operation the sidebar's views button uses, and the popup placement
  already flips above an anchor with no room below.
- Items whose action is not available (no host connected) are dimmed by the
  menu's existing availability rule (`IsMenuItemEnabled` →
  `MenuItemViewModel::enabled`), so the menu never offers a verb with nothing to
  act on and its shape is stable.
- `ParseMenuId` learns `"remote"`, so a plugin can contribute to the menu as it can
  to the others.

Out of scope: a Remote submenu per host (one host is the common case and the
commands prompt when there are several), moving the Changed on Host and Host
sidebar views (they are views, reached from this menu), and any change to the
commands themselves.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `remote-connection-lifecycle`: "The host entry points sit beside their local
  twins" becomes "The remote verbs live in a Remote menu": the File placement is
  removed, the welcome button and the Terminal-menu local terminal are kept, and
  the status segment's click is specified.

## Impact

- `src/workspace/registries/WorkspaceMenuRegistry.*`: `MenuId::Remote`,
  `kRemoteItems`, the File removal, menu-label overrides, `ParseMenuId("remote")`.
  `IsMenuBarTopLevelMenu` is an exclusion list, so the new id is on the bar, in
  the overflow chevron and in the Compact hamburger with no further change.
- `src/workspace/services/StatusBarService.h`: `StatusBarSegmentValue::menu`
  (`MenuId`, `None` by default); `clickable()` is "has a command or a menu".
- `src/workspace/services/RemoteHostService.cpp` (`PublishStatusSegment`): the
  segment's `command` becomes `menu = MenuId::Remote`, tooltip "Remote menu".
- `src/workspace/shell/WorkspaceShellMouse.cpp` (the status-bar click): a segment
  with a menu opens it anchored at the segment's rect through the existing
  anchored-menu path (`ComputePopupMenuRect` flips above the status bar on its
  own); a segment with a command runs it as today.
- `src/workspace/control/ControlUiShellSources.cpp`: the segment reports as a
  button that opens a menu (so QA can click it and assert the menu).
- Tests: menu registry (the Remote menu's items and order, File without the twin,
  labels without the prefix, palette labels with it, each menu item's id has an
  availability case), layout (the menu bar still fits at the narrowest Normal
  width, else overflows with the chevron, never truncates), the segment click
  opening the popup above the bar and Escape closing it, and a `qa` scenario
  driving the Remote menu from the menu bar and from the segment.
- Docs: `remote-projects.md` § 7.1's entry-point table and § 7.4 (the click:
  the Remote menu, not the palette filter it designed nor the status toast as
  built), the comment in `WorkspaceChromeMouseCoordinator.cpp` that names "File >
  Remote: Open Folder on Host", Help/About if it lists menus.
- Sequencing: after `remove-host-terminal-command`; before `remote-host-activity`
  (whose Show Status and End verbs slot into this menu); independent of
  `remote-terminal-restore`.
