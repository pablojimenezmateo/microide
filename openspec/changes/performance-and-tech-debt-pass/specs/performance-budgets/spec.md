## ADDED Requirements

### Requirement: Render-Path Allocation Budget

The render and text-layout hot paths SHALL NOT perform heap allocations on a per-call basis for operations that execute every frame or on every key event. Specifically, `TextRenderer::TruncateToWidth` SHALL reuse a thread-local scratch buffer rather than allocating a new vector on each call, and the `TextRenderer` width cache SHALL be bounded to prevent unbounded memory growth.

#### Scenario: TextRenderer truncation call
- **WHEN** `TextRenderer::TruncateToWidth` is called during a frame render
- **THEN** it SHALL NOT allocate a new heap buffer for the boundary scratch space; it SHALL reuse a thread-local or pre-allocated buffer for that purpose

#### Scenario: TextRenderer width cache growth
- **WHEN** the `TextRenderer` width cache reaches its configured maximum entry count
- **THEN** the cache SHALL be reset or evicted so that memory use remains bounded, and the reset SHALL NOT cause a performance spike visible in frame timing

### Requirement: Incremental Build Cost Budget

The workspace layer SHALL maintain a bounded include footprint so that changing one workspace subsystem does not trigger a full rebuild of unrelated subsystems. `WorkspaceShell.h` SHALL include fewer than 40 direct headers, and layout types referenced only by mouse coordinators SHALL be separated into a dedicated low-footprint header.

#### Scenario: Mouse coordinator compile dependency
- **WHEN** a mouse coordinator header (such as `WorkspaceTabMouseCoordinator.h` or `WorkspaceCompareMouseCoordinator.h`) is included in a translation unit
- **THEN** it SHALL NOT transitively pull in the full `WorkspaceShell.h` header and its 70+ transitive dependencies

#### Scenario: Workspace-layer incremental rebuild
- **WHEN** a change modifies only a single workspace coordinator implementation file
- **THEN** the set of translation units that require recompilation SHALL be limited to that coordinator and its direct dependents, not the entire workspace layer
