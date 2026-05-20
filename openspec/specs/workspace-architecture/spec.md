# workspace-architecture Specification

## Purpose
Define the workspace architecture boundaries that keep the shell small, service-owned, and
render-safe. The spec captures the durable invariants for coordinator dependencies, service
ownership, view-model-driven rendering, plugin/runtime seams, persistence boundaries, and
architectural lint checks that prevent previously removed coupling from returning.

## Requirements
### Requirement: Workspace Shell Is A Thin Orchestrator

The `WorkspaceShell` type SHALL act as a thin orchestrator that owns service instances and routes input events to coordinators. It SHALL NOT be a sink for unrelated state, helpers, or behavior. The shell header SHALL contain no `friend class` declarations, and external types SHALL NOT mutate shell-private state.

#### Scenario: Shell size invariant
- **WHEN** the source tree is built
- **THEN** `src/workspace/WorkspaceShell.h` SHALL be no larger than 400 lines and `src/workspace/WorkspaceShell.cpp` SHALL be no larger than 600 lines, enforced by an architectural-lint test in `microide_tests`

#### Scenario: No friend access
- **WHEN** any file under `src/workspace/` is parsed
- **THEN** the architectural-lint test SHALL fail if any `friend class` or `friend struct` declaration is present in workspace source files

### Requirement: Coordinators Consume Service Interfaces

Workspace coordinators SHALL receive their dependencies as narrow service-interface references through constructor injection. Coordinators SHALL NOT take a `WorkspaceShell&`, hold a pointer to the shell, or reach into shell-private state.

#### Scenario: Coordinator construction surface
- **WHEN** a new or modified coordinator under `src/workspace/Workspace*Coordinator*.h` is compiled
- **THEN** its constructor SHALL accept only service-interface references, value-typed input state, and read-only resource handles, and the architectural-lint test SHALL reject any `WorkspaceShell&` or `WorkspaceShell*` constructor parameter

#### Scenario: Mutation through services only
- **WHEN** a coordinator needs to mutate workspace state
- **THEN** it SHALL call a method declared on a service interface, and the service SHALL be the sole owner of the mutated state

### Requirement: Service Boundaries For Workspace Subsystems

The workspace SHALL expose a closed set of service interfaces that own their state and define the only mutation API for that state. The minimum service set SHALL include `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `PersistenceService`, `LayoutModeService`, `StatusBarService`, and `SettingsOverlayService`.

#### Scenario: Editor tab mutation routes through EditorTabService
- **WHEN** any caller opens, closes, splits, activates, or saves an editor tab
- **THEN** it SHALL do so through `EditorTabService` and SHALL NOT manipulate tab vectors, active indices, or split trees directly

#### Scenario: Persistence routes through PersistenceService
- **WHEN** workspace state, user configuration, and session restore data is read or written
- **THEN** it SHALL route through `PersistenceService`, and no other type SHALL open files in the workspace state directory

#### Scenario: Layout mode is a single service
- **WHEN** any caller reads or writes the active `LayoutMode`
- **THEN** it SHALL do so through `LayoutModeService`, and no caller other than `ComputeLayout` SHALL recompute the breakpoint independently

#### Scenario: Status bar mutation routes through StatusBarService
- **WHEN** any caller updates a status-bar segment value (project label, branch, line/column, problems count, LSP state, encoding, indent display, language)
- **THEN** it SHALL do so through `StatusBarService`, and the status-bar render TU SHALL read only the view model produced from that service

#### Scenario: Settings overlay mutation routes through SettingsOverlayService
- **WHEN** the settings overlay opens, filters, edits, or persists a setting
- **THEN** it SHALL do so through `SettingsOverlayService`, which SHALL be the only consumer of the overlay's mutable state, and the new render TU SHALL consume only its view model

### Requirement: Render Functions Take View Models

Workspace render functions SHALL consume explicit POD-like view-model structs produced by a `RenderViewModelBuilder`. Render code SHALL NOT call shell helpers, query coordinators, or read mutable state during a draw pass.

#### Scenario: Render-time shell access is forbidden
- **WHEN** any function in `src/workspace/WorkspaceShellRender*.cpp` is compiled
- **THEN** it SHALL accept its inputs as view-model parameters, and the architectural-lint test SHALL reject calls into `WorkspaceShell` member functions other than view-model-builder entry points

#### Scenario: View models are allocation-free in the hot path
- **WHEN** a view model is consumed by a render function
- **THEN** consumption SHALL not allocate; any required allocations SHALL happen in the builder, with caching governed by performance tests

### Requirement: Active Editor Viewport Has A Single Owner

The active editor viewport SHALL be owned exclusively by the active editor tab. There SHALL be no project-level or shell-level fallback viewport for normal editor operations.

#### Scenario: text_viewport_ is removed
- **WHEN** the source tree is searched
- **THEN** the symbols `text_viewport_` and `current_project_state_.text_viewport` SHALL NOT exist, and a welcome/placeholder surface SHALL own its own dedicated model

#### Scenario: All edit paths use ActiveEditorViewport
- **WHEN** an editor action runs
- **THEN** it SHALL resolve its target viewport through `EditorTabService::ActiveViewport()` (or equivalent service API) and SHALL NOT consult any shell-level viewport alias

### Requirement: Plugin Host Is Decomposed

`PluginHost` SHALL be decomposed into a runtime core plus per-surface extension modules (commands, sidebars, syntax, diagnostics, hover, auth, providers, lifecycle). Each module SHALL own its registry or runtime seam and SHALL NOT exceed 800 lines.

#### Scenario: PluginHost size invariant
- **WHEN** the source tree is built
- **THEN** no single `src/plugin/*.cpp` translation unit SHALL exceed 800 lines, enforced by the architectural-lint test

#### Scenario: Lua VM lifecycle is isolated
- **WHEN** plugin runtime work creates, suspends, or destroys a Lua VM
- **THEN** that work SHALL go through one `LuaRuntime` seam owned by the runtime core; no extension-surface module SHALL hold a raw `lua_State*`

### Requirement: Architectural Invariants Are Enforced By CI

The build SHALL include an architectural-lint test that runs as part of `ctest` and rejects regressions of these invariants without requiring human review.

#### Scenario: Lint is part of the default test run
- **WHEN** `ctest --test-dir build` runs
- **THEN** the architectural-lint test SHALL execute and SHALL fail the run if any banned pattern (new `friend class` in workspace, new `WorkspaceShell&` in coordinator, `try`/`catch` numeric parsing, oversized translation unit) is introduced

### Requirement: Render Surface Lint Coverage Is Discovery-Based

The architectural-lint test SHALL discover every `src/workspace/WorkspaceShellRender*.cpp` translation unit automatically and apply the render-time-shell-access checks to all of them, without requiring a hand-maintained file list.

#### Scenario: New render translation unit is automatically covered
- **WHEN** a new file matching `src/workspace/WorkspaceShellRender*.cpp` is added
- **THEN** the architectural-lint test SHALL include it in the render-time-shell-access checks without further configuration, and SHALL fail if the file reads `context_.current_project_state` or calls `CurrentTextInputSurface(...)`

### Requirement: Coordinator Translation Units Are Size-Capped

No `src/workspace/Workspace*Coordinator*.cpp` translation unit SHALL exceed 800 lines. The architectural-lint test SHALL hard-fail on a violation.

#### Scenario: Oversized coordinator is rejected
- **WHEN** a coordinator translation unit grows beyond 800 lines
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the file in the failure message

### Requirement: View Models Hold No Back-References

A type whose name ends in `ViewModel` SHALL NOT contain a field of type `WorkspaceShell*`, `WorkspaceShell&`, any type whose name ends in `Coordinator`, or any type whose name ends in `Service`. The architectural-lint test SHALL reject such fields.

#### Scenario: View-model back-reference is rejected
- **WHEN** a `<Surface>ViewModel` struct adds a field referencing the shell, a coordinator, or a service
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the offending field

### Requirement: Workspace-State File I/O Is Owned By PersistenceService

No file outside `src/workspace/PersistenceService.{h,cpp}`, the one-shot legacy importer (until removed in the scheduled follow-up), and `src/persistence/*` SHALL open files matching the project-state, user-config, workspace-session, or conversation-registry filename patterns. The architectural-lint test SHALL enforce the boundary.

#### Scenario: Direct file I/O for workspace state is rejected
- **WHEN** a translation unit outside the allowed set opens a file matching the documented patterns
- **THEN** the architectural-lint test SHALL hard-fail and SHALL identify the offending file and call site

### Requirement: Plugin Translation Unit Size Is Hard-Fail

The plugin-translation-unit-size rule (`src/plugin/*.cpp` ≤ 800 lines) SHALL be flipped from soft-fail to hard-fail.

#### Scenario: Oversized plugin translation unit is rejected
- **WHEN** a plugin translation unit exceeds 800 lines
- **THEN** the architectural-lint test SHALL hard-fail (no longer a warning)

### Requirement: Throwing Numeric Parse Detection Uses A Tokenizing Scan

The architectural-lint check for `try`/`std::sto*` parsing SHALL use a tokenizing scan that walks `try { ... }` block bodies properly, replacing the current heuristic that can miss multi-line and nested cases.

#### Scenario: Multi-line try/sto pattern is caught
- **WHEN** a `try` block whose body spans multiple statements contains a call to any `std::sto*` function
- **THEN** the architectural-lint test SHALL detect the pattern and SHALL hard-fail with the file and line number

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

### Requirement: New Workspace Coordinators Use Service Interfaces Only

`LayoutModeService`, `StatusBarService`, and `SettingsOverlayService` SHALL be added without enlarging `WorkspaceShell`. The shell file-size invariants (`WorkspaceShell.h` ≤ 400 lines, `WorkspaceShell.cpp` ≤ 600 lines) SHALL continue to hold after this change. Coordinator constructors that touch the new services SHALL accept service-interface references only and SHALL NOT take `WorkspaceShell&`/`WorkspaceShell*`.

#### Scenario: Shell file-size invariants still hold
- **WHEN** the source tree is built after the new services are added
- **THEN** the architectural-lint test SHALL re-assert that `WorkspaceShell.h` ≤ 400 lines and `WorkspaceShell.cpp` ≤ 600 lines, and SHALL fail if any new service is reached through a shell back-reference

#### Scenario: New coordinator constructors take services
- **WHEN** a new coordinator that consumes one of the three new services is constructed
- **THEN** its constructor parameter list SHALL contain service-interface references and SHALL NOT contain `WorkspaceShell&` or `WorkspaceShell*`, enforced by the architectural-lint test

### Requirement: New Render Translation Units Are View-Model Driven

The new render TUs introduced by this change (`WorkspaceShellRenderStatusBar.cpp`, `WorkspaceShellRenderSettingsOverlay.cpp`, and any new menu-overflow render unit) SHALL consume `RenderViewModelBuilder` view models and SHALL be added to the architectural-lint render-surface coverage list.

#### Scenario: Render-surface lint covers new TUs
- **WHEN** the architectural-lint test runs after this change archives
- **THEN** the new render TUs SHALL be covered by the discovery-based render-lint scan, and any future TU added under `src/workspace/WorkspaceShellRender*.cpp` SHALL be covered automatically

#### Scenario: Status-bar render TU does not allocate in steady state
- **WHEN** the status-bar render TU repaints with no segment-content change
- **THEN** no string materialization (`std::string(...)`, `+`, `+=`, `to_string`, `format`) SHALL occur in the TU, mirroring the existing render-hot-path rule

### Requirement: Hotspot-Prone Workspace Paths Expose Measurable Seams
Workspace subsystems that repeatedly appear in hotspot audits SHALL expose deterministic measurement seams through existing services, builders, or harness hooks rather than embedding opaque work in event-loop glue or render translation units.

#### Scenario: Hotspot found in opaque shell-thread path
- **WHEN** the audit identifies a hotspot that cannot be isolated by current harness instrumentation
- **THEN** the owning subsystem SHALL add a narrow measurable seam so the path can be validated by automated perf scenarios without broad shell coupling

### Requirement: Optimization Refactors Preserve Ownership Boundaries
Performance-motivated workspace refactors SHALL preserve service ownership and coordinator boundaries, and SHALL NOT reintroduce broad `WorkspaceShell` reach-through to reduce short-term code churn.

#### Scenario: Optimization proposes shell reach-through shortcut
- **WHEN** an optimization change attempts to bypass a service boundary by mutating state directly through shell internals
- **THEN** the change SHALL be rejected in favor of a service-level API that keeps the path measurable and testable
