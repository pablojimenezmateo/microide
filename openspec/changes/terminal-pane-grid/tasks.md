## 1. Shared grid

- [ ] 1.1 Rename `EditorSplitTree` → `SplitTree` (+ orientation / node / record types, file), `EditorBodyDrop*` → `PaneDrop*`
- [ ] 1.2 `SplitTree::GraftTree` (canonical graft) with tests
- [ ] 1.3 `SplitTreeLayout`: `ComputeSplitRegions`, `SplitRegionIndexAt`, `AdjacentSplitRegion`; editor rects and neighbours delegate
- [ ] 1.4 `kMaxSplitLeaves`; `kMaxEditorGroups` / `kMaxTerminalPanes` alias it

## 2. Terminal grid

- [ ] 2.1 `TerminalTabState` holds a `SplitTree`; delete `TerminalPaneLayout.{h,cpp}` and its weights
- [ ] 2.2 Split right / down, close, directional focus (4 ways), divider drag on both axes + cursor + render
- [ ] 2.3 Actions: `term-split-down`, `term-pane-up`, `term-pane-down` (+ keybindings, menus, availability)
- [ ] 2.4 Persistence: terminal tab split tree record; restore + capture (dropped own-launcher panes removed from the tree)
- [ ] 2.5 Control channel: terminal dividers and tree in the queries

## 3. Drag a terminal tab onto the body

- [ ] 3.1 Drag state: body drop serves both kinds; terminal target tab recorded at press
- [ ] 3.2 Coordinator: show the previous tab while over the body, resolve edge zones, commit graft through `TerminalPanelService::JoinTabIntoPane`
- [ ] 3.3 Overlay paint + damage for the panel body
- [ ] 3.4 Tests: drop right / bottom, cap refusal, return to strip, cancel

## 4. Consistency, docs, validation

- [ ] 4.1 Editor drag-to-split audit against the terminal path; fix or record TDs
- [ ] 4.2 Docs: implementation guide, CLAUDE/AGENTS invariant wording, known-tech-debt
- [ ] 4.3 Full test shards, clang-build, asan, qa
