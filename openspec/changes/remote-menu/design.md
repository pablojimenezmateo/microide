## Context

The menu bar is File, Edit, View, Go, Git, Terminal, Debug, Help
(`WorkspaceMenuSpecs`), each a static `MenuSpec` over `MenuItemSpec`s that may
override the action's label (`MenuItem(ActionId::CompareCopyHunkPatch, "Copy
Change as Patch")`). Narrow windows get an overflow chevron whose popup lists the
omitted menus; Compact mode replaces the bar with one hamburger popup
(`responsive-shell-layout`). Status segments carry a `command` string and are
clickable iff it is non-empty (`StatusBarSegmentValue`); the remote segment's is
empty. Fifteen remote `ActionSpec`s carry `Remote: …` palette labels.

## Goals / Non-Goals

**Goals:**
- Every remote verb a user can take without typing an argument is in one menu.
- Menu labels read as the menu's items; palette labels keep their category.
- The host's status segment is the shortcut to that menu.
- No new menu machinery: a `MenuSpec`, label overrides, the existing popup.

**Non-Goals:**
- Per-host submenus; changing any command's behaviour; moving sidebar views.

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
reached today. Accelerators: none new; the palette remains the keyboard path.

*Alternative:* keep the twins in File and Terminal as well. Rejected by the
proposal: one place, no duplicates; the welcome page keeps the cold-start twin
because it is not a menu and a first-time user sees it before any menu.

### D2. The status segment opens the menu as an anchored popup
`PublishStatusSegment` sets the remote segment's `command` to `menu-open remote`
(a command the chrome mouse coordinator already understands for the overflow
chevron's and hamburger's popups — or the one it gains, see tasks), and the
coordinator anchors the popup above the segment instead of below a menu-bar item.
Tooltip: "Remote menu". The segment's text keeps naming the host and its state,
which satisfies "Clickable Status Segments Expose Intent": the destination is the
menu named by the segment's own subject.

*Alternative:* click runs `Show Status`. Rejected: one verb hard-wired into the
bar when the menu holds eleven; VS Code's indicator opens the list for the same
reason.

### D3. Availability dims, it does not hide
A menu item whose `WorkspaceActionAvailability` answer is "not now" (no host
connected: Reconnect, Disconnect, Stop Host Server, Show Host Server Log, Show
Status, the port verbs, Remove Mirror) is drawn dimmed and inert, the way the Git
menu treats its verbs with no repository, so the menu's shape is stable and a user
learns where things are before needing them. Open Folder on Host… is always
available.

### D4. Width is the overflow chevron's problem, measured once
One more top-level label (`Remote`, six glyphs) at the narrowest Normal-mode
width either fits or overflows into the chevron; neither truncates. A layout test
pins the width at which the chevron first appears so a later menu knows the budget.

## Risks / Trade-offs

- [Muscle memory for File > Remote: Open Folder on Host…] → The menu is new and
  named; the welcome button and the palette entry are unchanged.
- [Dimmed items read as broken] → Same convention as Git with no repository; the
  tooltip on a dimmed item says "Connect to a host first" where the renderer
  supports item tooltips, else nothing.

## Migration Plan

UI-only; nothing persisted changes. The `qa` scenarios that drive File > Remote:
Open Folder on Host… by menu (if any) move to the Remote menu.

## Open Questions

None.
