## 1. The menu

- [ ] 1.1 `MenuId::Remote` and `kRemoteItems` (D1 order and separators) between Git and Terminal; remove `RemoteOpenFolder` from `kFileItems`; menu-label overrides without the `Remote:` prefix; `ActionSpec` palette labels unchanged; `ParseMenuId("remote")`
- [ ] 1.2 Availability: every menu-present remote id answers precisely in `WorkspaceActionAvailability` (connected host present, else "not now"; Open Folder on Host… always), so the existing `IsMenuItemEnabled` dims them
- [ ] 1.3 Registry test: the Remote menu's items and order; File without the twin; every menu label prefix-free; every palette label prefixed; each menu item's id has an availability case; `ParseMenuId("remote")`

## 2. The status segment

- [ ] 2.1 `StatusBarSegmentValue::menu` (`MenuId`); `clickable()` = command or menu; `PublishStatusSegment` sets `menu = MenuId::Remote`, no command, tooltip "Remote menu"; the status-bar click path opens a segment's menu anchored at the segment's rect through the existing anchored-menu operation; the control channel's ui listing reports the segment as a menu button
- [ ] 2.2 Test: clicking the segment opens the Remote popup above the status bar; Escape closes it; a click on an item runs it; a segment with a command still runs the command

## 3. Width and QA

- [ ] 3.1 Layout test: at the narrowest Normal-mode width the bar fits or shows the chevron, never truncates; pin the width at which the chevron first appears
- [ ] 3.2 `qa` scenario: open the Remote menu from the menu bar (the `menu-bar/Remote` ui element), walk its items with the arrow keys, run Show Status; click the status segment and assert the same popup; both under the Xvfb and Wayland lanes
- [ ] 3.3 `tools/run-checks.sh tests` (batches of 16), `qa`, `qa-wayland`, `clang-build`

## 4. Docs

- [ ] 4.1 `remote-projects.md` § 7.1 entry-point table (a Remote menu and the segment) and § 7.4 (the click opens the Remote menu), the `WorkspaceChromeMouseCoordinator.cpp` comment naming "File > Remote: Open Folder on Host", Help/About if it lists menus, `active-work.md`
