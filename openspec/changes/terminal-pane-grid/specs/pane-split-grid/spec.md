## ADDED Requirements

### Requirement: Editor And Terminal Panes Share One Split Grid
The editor area's groups and each terminal tab's panes SHALL be arranged by the same split-tree type: an n-ary tree whose in-order leaves are the panes, laid out by one region walk that reports dividers addressed by node and boundary. Each surface SHALL hold at most `kMaxSplitLeaves` panes.

#### Scenario: A terminal pane split downward
- **WHEN** the user runs Split Terminal Down on a terminal tab with one pane
- **THEN** the tab SHALL show two panes stacked vertically, the new one below and active, divided by a horizontal divider that resizes vertically

#### Scenario: Directional terminal focus follows the screen
- **WHEN** a terminal tab is split into a left pane and a right column of two stacked panes and the bottom-right pane is active
- **THEN** Focus Terminal Pane Up SHALL activate the top-right pane and Focus Terminal Pane Left SHALL activate the left pane

### Requirement: A Terminal Tab Dropped On A Pane Edge Splits That Pane
Dragging a terminal strip tab off the strip and over the terminal body SHALL show the tab that was on screen before the press, and SHALL offer the left, right, top and bottom fifth of the pane under the pointer as drop zones with the same preview as the editor's. Dropping SHALL move every pane of the dragged tab, in its own arrangement, onto that side of that pane and close the dragged tab.

#### Scenario: Two full-screen terminals side by side
- **WHEN** terminal tab A is on screen and the user drags tab B onto the right edge of A's body and releases
- **THEN** tab B SHALL be gone, tab A SHALL show A's pane on the left and B's pane on the right, and B's pane SHALL be active

#### Scenario: A drop that would exceed the cap
- **WHEN** the combined pane count of the dragged tab and the tab on screen exceeds `kMaxSplitLeaves`
- **THEN** no drop zone SHALL be offered and releasing SHALL change nothing

#### Scenario: Dragging back to the strip
- **WHEN** the pointer returns from the body to the strip during the same drag
- **THEN** the gesture SHALL be a strip reorder again and the dragged tab SHALL be shown

### Requirement: Terminal Split Layout Persists As A Tree
A saved session SHALL record each terminal tab's split tree in the same pre-order form as the editor grid, and a restore SHALL rebuild the same arrangement when the tree's leaf count matches the revived panes, falling back to an even row otherwise.

#### Scenario: Round trip of a grid
- **WHEN** a project with a terminal tab split right and then down is closed and reopened
- **THEN** the tab SHALL revive with the same arrangement and shares
