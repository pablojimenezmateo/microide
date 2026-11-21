# Spec Delta

## MODIFIED Requirements

### Requirement: Render Functions Take View Models

Workspace render functions SHALL consume explicit POD-like view-model structs produced by a `RenderViewModelBuilder`. Render code SHALL NOT call shell helpers, query coordinators, or read mutable state during a draw pass. A surface migrated to derived damage (`frame-damage-derivation`) SHALL take its view model as the frame's paint snapshot, so that the snapshot is the complete set of its paint inputs.

#### Scenario: Render-time shell access is forbidden
- **WHEN** any function in `src/workspace/WorkspaceShellRender*.cpp` is compiled
- **THEN** it SHALL accept its inputs as view-model parameters, and the architectural-lint test SHALL reject calls into `WorkspaceShell` member functions other than view-model-builder entry points

#### Scenario: View models are allocation-free in the hot path
- **WHEN** a view model is consumed by a render function
- **THEN** consumption SHALL not allocate; any required allocations SHALL happen in the builder, with caching governed by performance tests

#### Scenario: A migrated surface's view model is its paint snapshot
- **WHEN** a surface is migrated to derived damage
- **THEN** its render function reads nothing but its snapshot and immutable resources, and the lint enforces it for that surface
