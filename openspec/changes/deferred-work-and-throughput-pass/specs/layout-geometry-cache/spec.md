## ADDED Requirements

### Requirement: WorkspaceLayout Is Cached With Dirty Tracking

`WorkspaceLayout` SHALL be computed at most once per frame, and SHALL NOT be computed when the geometry inputs (window size, divider positions, sidebar visibility, panel height) have not changed since the last computation. A dirty flag SHALL be set by resize, divider-drag, and sidebar/panel-toggle event handlers, and SHALL be cleared after `ComputeLayout()` completes.

#### Scenario: Layout is not recomputed on frames with no geometry change
- **WHEN** the user is typing or scrolling with no window resize, divider drag, or sidebar toggle since the previous frame
- **THEN** `ComputeLayout()` SHALL NOT be called during `PrepareFrameOnce`

#### Scenario: Layout is recomputed after a resize event
- **WHEN** the user resizes the window or drags a divider
- **THEN** the dirty flag SHALL be set by the event handler; on the next `PrepareFrameOnce`, `ComputeLayout()` SHALL be called once and the cache updated

#### Scenario: Layout is always computed at least once before first render
- **WHEN** the application starts and no geometry event has yet been received
- **THEN** the dirty flag SHALL default to `true`, ensuring `ComputeLayout()` runs on the first `PrepareFrameOnce` call before any `RenderClip` call executes

### Requirement: Visible-Line Range Is Pre-Computed Once Per Frame

`PrepareFrameOnce` SHALL compute the active editor viewport's visible-line range exactly once per frame and store it in the `FrameToken`. All render phases that consume the visible-line range SHALL read it from the `FrameToken` and SHALL NOT recompute it independently.

#### Scenario: All render phases read the same pre-computed visible-line range
- **WHEN** a frame renders the editor surface, the gutter, and the blame overlay
- **THEN** each phase SHALL read the visible-line range from the `FrameToken` rather than calling into `EditorTabService::ActiveViewport()` independently

#### Scenario: Visible-line range is absent when no editor tab is active
- **WHEN** the active surface is a compare tab, terminal, or other non-editor surface
- **THEN** the `FrameToken` SHALL carry an empty or sentinel visible-line range; render phases that depend on it SHALL be structurally gated by the presence of an editor view model, not by a runtime null-check

### Requirement: Tab-Strip Geometry Is Cached Per Geometry-Key

Tab-strip layout (tab widths, positions, overflow scroll offset) SHALL be cached against a geometry-key comprising the tab count, window width, and active tab index. The cache SHALL be invalidated only when the key changes.

#### Scenario: Tab-strip layout is not recomputed when scrolling the editor
- **WHEN** the user scrolls the editor content and the tab count, window width, and active tab index are unchanged
- **THEN** the tab-strip layout SHALL be served from the cache without recomputation

#### Scenario: Tab-strip layout is recomputed when a tab is added or closed
- **WHEN** the user opens or closes a tab, changing the tab count
- **THEN** the geometry-key changes and the tab-strip layout SHALL be recomputed on the next frame
