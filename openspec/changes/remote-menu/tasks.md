## 1. The menu

- [x] 1.1 `MenuId::Remote` and `kRemoteItems` (D1 order and separators) between Git and Terminal; remove `RemoteOpenFolder` from `kFileItems`; menu-label overrides without the `Remote:` prefix; `ActionSpec` palette labels unchanged; `ParseMenuId("remote")`
- [x] 1.2 Availability: every menu-present remote id answers precisely in `WorkspaceActionAvailability` (connected host present, else "not now"; Open Folder on Host… always), so the existing `IsMenuItemEnabled` dims them
- [x] 1.3 Registry test: the Remote menu's items and order; File without the twin; every menu label prefix-free; every palette label prefixed; each menu item's id has an availability case; `ParseMenuId("remote")`

## 2. The status segment

- [x] 2.1 `StatusBarSegmentValue::menu` (`MenuId`); `clickable()` = command or menu; `PublishStatusSegment` sets `menu = MenuId::Remote`, no command, tooltip "Remote menu"; the status-bar click path opens a segment's menu anchored at the segment's rect through the existing anchored-menu operation; the control channel's ui listing reports the segment as a menu button
- [x] 2.2 Test: clicking the segment opens the Remote popup above the status bar; Escape closes it; a click on an item runs it; a segment with a command still runs the command

## 3. Width and QA

- [x] 3.1 Layout test: at the narrowest Normal-mode width the bar fits or shows the chevron, never truncates; pin the width at which the chevron first appears (every menu fits from 720px, the narrowest Normal width, so the chevron never shows outside Compact; the menus' extent, 628px, is pinned instead)
- [x] 3.2 `qa` scenario: open the Remote menu from the menu bar (the `menu-bar/Remote` ui element), walk its items with the arrow keys, run Show Status; click the status segment and assert the same popup; both under the Xvfb and Wayland lanes
- [x] 3.3 `tools/run-checks.sh tests` (batches of 16), `qa`, `qa-wayland`, `clang-build`

## 4. Docs

- [x] 4.1 `remote-projects.md` § 7.1 entry-point table (a Remote menu and the segment) and § 7.4 (the click opens the Remote menu), the `WorkspaceChromeMouseCoordinator.cpp` comment naming "File > Remote: Open Folder on Host", Help/About if it lists menus, `active-work.md`

## 5. Found while building it

- [x] 5.1 Menu-run pickers: Stop Forwarding a Port… lists the forwards; Remove Mirror… lists closed mirrors (usage `<root>` required); host verbs ask which host when several are open (D5)
- [x] 5.2 `ParseMenuId` derived from the spec table; the plugin parser's menu list held to the bar by a test; `MenuId` in its own header with `kMenuIdCount`
