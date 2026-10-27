## 1. The menu

- [ ] 1.1 `MenuId::Remote` and `kRemoteItems` (D1 order and separators) between Git and Terminal; remove `RemoteOpenFolder` from `kFileItems`; menu-label overrides without the `Remote:` prefix; `ActionSpec` palette labels unchanged
- [ ] 1.2 Availability: every menu-present remote id answers in `WorkspaceActionAvailability`; verify the menu renderer dims an unavailable item as the Git menu does, else add that one rule
- [ ] 1.3 Registry test: the Remote menu's items and order; File without the twin; every menu label prefix-free; every palette label prefixed; each menu item's id has an availability case

## 2. The status segment

- [ ] 2.1 The remote segment's `command` opens the Remote menu as a popup anchored at the segment (the overflow/hamburger popup, anchored upward); tooltip "Remote menu"
- [ ] 2.2 Test: clicking the segment opens the popup with the Remote items; Escape closes it; a click on an item runs it

## 3. Width and QA

- [ ] 3.1 Layout test: at the narrowest Normal-mode width the bar fits or shows the chevron, never truncates; pin the width at which the chevron first appears
- [ ] 3.2 `qa` scenario: open the Remote menu by keyboard (Alt+R or the menu-bar arrows), walk its items, run Show Status; the segment click under Xvfb and Wayland lanes
- [ ] 3.3 `tools/run-checks.sh tests` (batches of 16), `qa`, `qa-wayland`, `clang-build`

## 4. Docs

- [ ] 4.1 `remote-projects.md` § 7 entry-point table (a Remote menu and the segment), the `WorkspaceChromeMouseCoordinator.cpp` comment naming "File > Remote: Open Folder on Host", Help/About if it lists menus, `active-work.md`
