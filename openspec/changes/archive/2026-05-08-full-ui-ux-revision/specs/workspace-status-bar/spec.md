## MODIFIED Requirements

### Requirement: Status Bar Segments Are Host-Owned And Click-Routable
Status-bar segments SHALL be defined by a closed enum owned by the host. Each segment SHALL declare label text, an optional icon glyph, an optional click target action, and a tooltip. Plugins SHALL NOT add status-bar segments through the host; plugin contributions to status remain limited to the existing notification and diagnostics surfaces.

#### Scenario: Segment list at first slice
- **WHEN** the status bar is built
- **THEN** the segment list SHALL include, in order from left: project name + branch + working-tree cleanliness state, language, indent display (tab/space + width), encoding, line/column, problems count (errors + warnings), LSP state, layout-mode badge — and any segment whose data is unavailable SHALL be omitted (not rendered as empty)

#### Scenario: Click on a clickable segment
- **WHEN** the user clicks on a segment that declares a click target action
- **THEN** the host SHALL route the click through `WorkspaceActionCoordinator` to that action (for example, source-control status opens the Source Control sidebar mode; language opens the language picker; problems opens the Problems sidebar mode)

#### Scenario: Tooltip on hover
- **WHEN** the user hovers a segment for the configured tooltip delay
- **THEN** a tooltip SHALL render with the segment's full text, mirroring the existing chrome-tab tooltip pattern

### Requirement: Status Bar Adapts To Layout Mode
In `Compact` mode the status bar SHALL keep the same vertical height but SHALL drop low-priority segments to fit. The drop order SHALL be deterministic and documented.

#### Scenario: Compact-mode segment drop order
- **WHEN** `LayoutMode` is `Compact` and the status bar's segments would not all fit horizontally
- **THEN** the host SHALL drop segments in this order until the row fits: layout-mode badge, encoding, language, indent display — and SHALL keep project+branch+cleanliness, line/column, problems count, and LSP state visible at all sizes ≥ `ui.layout_compact_breakpoint_px / 2`

#### Scenario: Status bar never overlaps the bottom panel
- **WHEN** the bottom panel is visible and the status bar is enabled
- **THEN** the layout SHALL place the status bar strictly below the bottom panel and the bottom-panel resize handle, with no overlap, and the bottom-panel-height clamp SHALL account for `kWorkspaceStatusBarHeight`
