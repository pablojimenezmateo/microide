## ADDED Requirements

### Requirement: Project-Scoped Workspace State Has Project Ownership

Workspace state that varies by project or influences project-local workflows SHALL be owned by project state/services and persisted as project-scoped data. `WorkspaceShell` SHALL NOT hold shell-global copies of wrap mode, ignored-tree expansion state, compare/merge divider fractions, or equivalent per-project UI state.

#### Scenario: Switching projects restores independent workspace state
- **WHEN** the user changes wrap mode, tree expansion, or compare divider placement in project A, switches to project B, and later returns to project A
- **THEN** project A SHALL restore its own saved values and project B SHALL retain its separate workspace state

#### Scenario: Project state mutates through services
- **WHEN** a workspace feature changes a project-scoped presentation value
- **THEN** the mutation SHALL route through the owning project service or persistence seam rather than through a shell-global alias

### Requirement: Pane Resize Limits Derive From Surface Viability

Workspace divider minima SHALL be derived from the minimum viable content of the participating surfaces and SHALL be computed by the owning layout/service code. Fixed shell-global clamps that prevent the sidebar, editor splits, compare panes, merge panes, or bottom panel from reaching their viable minimum SHALL NOT exist.

#### Scenario: Sidebar can shrink to its viable minimum
- **WHEN** the user drags the project-tree divider toward the window edge
- **THEN** the sidebar SHALL continue shrinking until it reaches the minimum width needed for viable tree interaction rather than stopping at an arbitrary earlier clamp

#### Scenario: Compare and merge panes can reach their viable minimums
- **WHEN** the user drags compare or merge dividers toward one side
- **THEN** each pane SHALL remain resizable down to the minimum width required to preserve its gutter and at least one visible text column
