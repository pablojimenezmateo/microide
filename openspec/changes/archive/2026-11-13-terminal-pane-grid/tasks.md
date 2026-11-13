## 1. Shared grid

- [x] 1.1 Rename `EditorSplitTree` → `SplitTree` (+ orientation / node / record types, file), `EditorBodyDrop*` → `PaneDrop*` (zone enum moved into `PaneDrop.h`, `SplitForPaneDropZone` replaces three copies of the zone → split mapping)
- [x] 1.2 `SplitTree::GraftTree` (canonical graft) with fixed and randomized tests
- [x] 1.3 `SplitTreeLayout`: `ComputeSplitRegions`, `SplitRegionIndexAt`, `AdjacentSplitRegion`, `SplitDividerRect::ShareAt`; editor rects, neighbours and divider drag delegate
- [x] 1.4 `kMaxSplitLeaves`; `kMaxEditorGroups` / `kMaxTerminalPanes` alias it

## 2. Terminal grid

- [x] 2.1 `TerminalTabState` holds a `SplitTree`; `TerminalPaneLayout.{h,cpp}` and its weights deleted
- [x] 2.2 Split right / down, close, directional focus (4 ways, geometric), divider drag on both axes + cursor + highlight
- [x] 2.3 Actions: `term-split-down`, `term-pane-up`, `term-pane-down`, `term-unsplit` (+ keybindings, menus, availability, README, man page)
- [x] 2.4 Persistence: `TerminalTabTag::Split` (the `Weight` tag retired); restore + capture (own-launcher panes removed from the tree); fuzz invariant
- [x] 2.5 Control channel: terminal dividers numbered like the editor's; `terminals` reports each tab's `split`

## 3. Drag a terminal tab onto the body

- [x] 3.1 Drag state: body drop serves both kinds; `terminal_drop_tab` recorded at press
- [x] 3.2 Coordinator: show the previous tab while over the body, resolve edge zones (no centre), commit graft through `TerminalPanelService::JoinTabIntoPane`; Escape / leaving the body shows the dragged tab
- [x] 3.3 Overlay paint (`DrawPaneDropPreview`, shared with the editor) in the bottom panel pass; damage through the existing body-drop path
- [x] 3.4 Tests: drop right / bottom, centre, back to strip, cap refusal, Escape; QA flow `wb-terminal-drag-split` under the redraw verifier

## 4. Consistency, docs, validation

- [x] 4.1 Editor audit: the held divider-drag cursor ignored a stacked split's axis (fixed, regression test); TD-436's Escape-then-release stale frame (tab drag never recorded the pointer; fixed, regression test); remaining gaps recorded as TD-2026-11-13-448
- [x] 4.2 Docs: implementation guide, terminal-emulation, CLAUDE.md invariant, CHANGELOG, known-tech-debt
- [x] 4.3 Full test shards, clang-build, asan, ubsan, qa (233/233 + monkey seed 1, run alone; under six concurrent lanes only latency budgets missed), perf-tests, fuzz (ProjectSessionRecordFuzz 90 s)
