## Why

Editor tabs can be dragged onto an editor pane's edge to split the editor area
left / right / top / bottom (VS Code's editor grid). Terminal tabs cannot: a
terminal tab's panes were a single side-by-side row (`TerminalPaneWeights`), the
only split verb was "Split Terminal" (to the right), and a terminal strip tab could
only be reordered. With two or three full-screen terminal tabs there was no way to
put two of them next to — or above — each other.

The two pane layouts were also two implementations of the same idea: the editor
had an n-ary, heap-free split tree (`EditorSplitTree`) with tree-walking layout,
divider addressing, persistence and directional neighbours; the terminal had a
flat weight vector with its own normalise / insert / remove / resize / layout /
hit-test helpers, its own divider rect type, its own persistence record and its
own tests.

## What Changes

- **One split tree.** `EditorSplitTree` becomes `SplitTree` (`workspace/SplitTree.h`),
  `EditorSplitOrientation` becomes `SplitOrientation`, and the tree gains
  `GraftTree` (insert a whole tree beside a leaf, canonicalised). The leaf cap is
  `kMaxSplitLeaves` (8); `kMaxEditorGroups` and `kMaxTerminalPanes` are both that
  cap (the terminal's was 6).
- **One region layout.** `ComputeSplitRegions` / `SplitRegionIndexAt` /
  `AdjacentSplitRegion` (`workspace/SplitTreeLayout.h`) carve any rect by a tree
  and report `SplitDividerRect`s addressed by (node, boundary). The editor wraps it
  with its per-pane chrome; the terminal uses it directly.
  `TerminalPaneLayout.{h,cpp}` and its weight helpers are deleted.
- **Terminal tabs are grids.** `TerminalTabState` holds a `SplitTree` parallel to
  its panes. New verbs: Split Terminal Down (`term-split-down`), Focus Terminal
  Pane Up / Down (`term-pane-up` / `term-pane-down`, Ctrl+Shift+Up/Down), and the
  existing Left / Right verbs become geometric (the pane on screen that way, not
  the next index). Dividers resize along either axis, with the matching cursor.
- **Drag a terminal tab onto the terminal body.** While a terminal tab is dragged
  out of the strip and over the panel body, the body shows the tab that was on
  screen before the press, and the pane under the pointer offers its four edges
  (VS Code's drop overlay, the same `PaneDropAt` rule as the editor). Dropping
  grafts the dragged tab — all its panes, in their own arrangement — onto that
  side of that pane and closes the dragged tab.
- **Shared drop vocabulary.** `EditorBodyDrop` / `EditorBodyDropZone` become
  `PaneDrop` / `PaneDropZone`; the drag state's body-drop fields serve both kinds.
- **Persistence.** A terminal tab record stores its split tree (the same
  pre-order record as the editor's). An older session's flat weights are not read;
  such a tab restores as an even row.
- **Control channel.** `ui` reports each terminal divider's orientation and node;
  the session/terminal query reports the tree.

## Impact

- `src/workspace/{SplitTree,SplitTreeLayout,PaneDrop}.{h,cpp}` (renamed / new),
  `WorkspaceLayout`, `TerminalPanelService`, `TabMouseCoordinator` drag,
  `PanelMouseCoordinator`, cursor, bottom-panel render, persistence, control
  sources, actions / keybindings / command registry.
- Tests: `SplitTreeTests` (was `EditorSplitTreeTests`), `SplitTreeLayoutTests`
  (replaces `TerminalPaneLayoutTests`), terminal shell tests for split down,
  directional focus, divider drag, and the tab-onto-body drop; persistence
  round-trip of a terminal tree.
