# workspace-status-bar Specification

## Purpose
TBD - created by archiving change responsive-layout-and-options-polish. Update Purpose after archive.
## Requirements
### Requirement: Persistent Status Bar Along The Window Bottom Edge

MicroIDE SHALL render a persistent host-owned status bar along the bottom edge of the workspace window, between the bottom-panel resize handle and the window's bottom edge, whenever `ui.show_status_bar` is true. The status bar SHALL be a single horizontal strip of fixed height computed from the current UI scale.

#### Scenario: Status bar is rendered when enabled
- **WHEN** `ui.show_status_bar` is true and the workspace has loaded a project
- **THEN** the layout pipeline SHALL reserve `kWorkspaceStatusBarHeight` pixels at the bottom of the content area, and the new status-bar render TU SHALL paint segments left-to-right and right-to-left from anchored ends

#### Scenario: Status bar is hidden when disabled
- **WHEN** `ui.show_status_bar` is false
- **THEN** the layout pipeline SHALL NOT reserve any height for the status bar and the status-bar render TU SHALL be a no-op

#### Scenario: View menu toggles visibility
- **WHEN** the user invokes the View → Status Bar action
- **THEN** the action SHALL flip `ui.show_status_bar`, persist the new value through `PersistenceService`, and trigger a layout-mode redraw

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

### Requirement: Status Bar Reads Through View Models

The status-bar render TU SHALL consume an explicit POD-like `StatusBarViewModel` produced by `RenderViewModelBuilder` and SHALL NOT call shell helpers, dereference services, or read mutable state during a draw pass, mirroring `workspace-architecture` rules.

#### Scenario: Render TU receives a view model
- **WHEN** the status bar is painted
- **THEN** it SHALL accept its inputs as a `StatusBarViewModel` parameter, and the architectural-lint test SHALL forbid `WorkspaceShell` member access from the new render TU other than view-model-builder entry points

#### Scenario: View-model build is allocation-free in the steady state
- **WHEN** the status bar is rebuilt every frame and no segment text has changed
- **THEN** the view-model builder SHALL reuse cached segment strings and SHALL NOT allocate, allocations being permitted only on segment-content change or layout-mode change

### Requirement: Status Bar Adapts To Layout Mode
In `Compact` mode the status bar SHALL keep the same vertical height but SHALL drop low-priority segments to fit. The drop order SHALL be deterministic and documented.

#### Scenario: Compact-mode segment drop order
- **WHEN** `LayoutMode` is `Compact` and the status bar's segments would not all fit horizontally
- **THEN** the host SHALL drop segments in this order until the row fits: layout-mode badge, encoding, language, indent display — and SHALL keep project+branch+cleanliness, line/column, problems count, and LSP state visible at all sizes ≥ `ui.layout_compact_breakpoint_px / 2`

#### Scenario: Status bar never overlaps the bottom panel
- **WHEN** the bottom panel is visible and the status bar is enabled
- **THEN** the layout SHALL place the status bar strictly below the bottom panel and the bottom-panel resize handle, with no overlap, and the bottom-panel-height clamp SHALL account for `kWorkspaceStatusBarHeight`

