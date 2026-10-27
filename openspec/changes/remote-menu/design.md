## Context

The menu bar is File, Edit, View, Go, Git, Terminal, Debug, Help
(`WorkspaceMenuSpecs`), each a static `MenuSpec` over `MenuItemSpec`s that may
override the action's label (`MenuItem(ActionId::CompareCopyHunkPatch, "Copy
Change as Patch")`). `IsMenuBarTopLevelMenu` is an exclusion list of the anchored
and context menus, so any other `MenuId` is a bar menu, overflows into the chevron
when the bar is narrow, and lists in Compact mode's hamburger
(`responsive-shell-layout`). A menu item's enabled state is already computed from
`WorkspaceActionAvailability` (`WorkspaceShell::IsMenuItemEnabled` →
`MenuItemViewModel::enabled`) and the coordinator skips disabled items when it
walks a menu. Anchored menus open through `open_anchored_menu(MenuId, rect)` (the
sidebar's views button, the tab and pane context menus), and
`ComputePopupMenuRect` flips a popup above its anchor when there is no room below.

Status segments carry a `command` string, are clickable iff it is non-empty, and
the status-bar click path (`WorkspaceShellMouse.cpp`) finds the segment under the
pointer and runs `ExecuteCommandName(command, {command_arg})`. The remote
segment's command is `remote-status` with tooltip "Remote: Show Status" (verified
2026-10-27 in `PublishStatusSegment`). Fourteen remote `ActionSpec`s carry
`Remote: …` palette labels after `remove-host-terminal-command`.

## Goals / Non-Goals

**Goals:**
- Every remote verb a user can take without typing an argument is in one menu.
- Menu labels read as the menu's items; palette labels keep their category.
- The host's status segment is the shortcut to that menu.
- No new menu machinery: a `MenuSpec`, label overrides, the existing anchored
  popup and the existing enabled rule.

**Non-Goals:**
- Per-host submenus; changing any command's behaviour; moving sidebar views;
  menu mnemonics (the bar has none today).

## Decisions

### D1. One static `MenuSpec`, after Git
`MenuId::Remote`, label `Remote`, between Git and Terminal: it is where work lives
(a host, like a repository) rather than a tool (a terminal). Items, with
separators:

```
Open Folder on Host…
---
Show Status                (→ the Host view once remote-host-activity lands)
Show Changed on Host
Show Host Server Log
---
Forward a Port…
Stop Forwarding a Port…
---
Reconnect
Disconnect
Stop Host Server
---
Remove Mirror…
```

Each is the existing `ActionId` with a label override that drops the prefix; the
`ActionSpec` labels (the palette, the control channel's `help`) are untouched.
`Resolve Conflict`, `Copy Command`, `Clear Changed on Host` and `Apply Held
Deletes` take arguments or belong to a row or a view and stay where they are
reached today. `New Local Terminal` (from `remove-host-terminal-command`) stays in
Terminal: it is a terminal verb that happens to matter in a remote project.
Accelerators: none new; the palette remains the keyboard path. `ParseMenuId`
gains `"remote"` so plugin contributions can target the menu like any other.

*Alternative:* keep the twin in File as well. Rejected by the proposal: one
place, no duplicates; the welcome page keeps the cold-start twin because it is
not a menu and a first-time user sees it before any menu.

### D2. The status segment carries a `MenuId`, and the click path anchors the menu at it
`StatusBarSegmentValue` gains `MenuId menu = MenuId::None`; `clickable()` becomes
`!command.empty() || menu != MenuId::None`. `PublishStatusSegment` sets `menu =
MenuId::Remote` and clears `command`; tooltip "Remote menu". In the status-bar
click path, a segment with a menu opens it through the shell's anchored-menu
operation with the segment's rect — the same call the sidebar's views button
makes — and `ComputePopupMenuRect` places it above the bar because there is no
room below. The segment's text keeps naming the host and its state, which
satisfies "Clickable Status Segments Expose Intent": the destination is the menu
named by the segment's own subject.

*Alternative 1:* a `menu-open <id>` command string on the segment. Rejected: the
command path has no rect to anchor at, so it would need a side channel for the
anchor, and a command that exists only to be a status-bar target is a hidden
coupling; a `MenuId` on the segment is data the host already owns.
*Alternative 2:* keep the click on `Show Status` (as built) or the palette
filtered to `Remote:` (as designed in § 7.4). Rejected: one verb hard-wired into
the bar when the menu holds ten, and the palette filter is the same list with a
text box in front; VS Code's indicator opens the list.

### D3. Availability dims, it does not hide — and it already does
A menu item whose `WorkspaceActionAvailability` answer is "not now" (no host
connected: Reconnect, Disconnect, Stop Host Server, Show Host Server Log, Show
Status, the port verbs, Remove Mirror) is drawn dimmed and inert by the existing
enabled rule, the way the Git menu treats its verbs with no repository, so the
menu's shape is stable and a user learns where things are before needing them.
Open Folder on Host… is always available. The work is making every menu-present
remote id answer availability precisely (today several "reject naming the host
they cannot find" at execution instead), not a renderer change.

### D4. Width is the overflow chevron's problem, measured once
One more top-level label (`Remote`, six glyphs) at the narrowest Normal-mode
width either fits or overflows into the chevron; neither truncates
(`kMaxMenuBarItems` is 24). A layout test pins the width at which the chevron
first appears so a later menu knows the budget.

## Risks / Trade-offs

- [Muscle memory for File > Remote: Open Folder on Host… and the segment's toast]
  → The menu is new and named; the welcome button and the palette entry are
  unchanged; Show Status is the second item of the menu the segment now opens.
- [Dimmed items read as broken] → Same convention as Git with no repository; the
  tooltip on a dimmed item says "Connect to a host first" where the renderer
  supports item tooltips, else nothing.

## Migration Plan

UI-only; nothing persisted changes. The `qa` scenarios that drive File > Remote:
Open Folder on Host… by menu (if any) move to the Remote menu; `qa-remote` sends
commands, so it is unaffected.

## Open Questions

None.
