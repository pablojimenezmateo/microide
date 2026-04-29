## ADDED Requirements

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

The workspace SHALL expose a closed set of service interfaces that own their state and define the only mutation API for that state. The minimum service set SHALL include `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, and `PersistenceService`.

#### Scenario: Editor tab mutation routes through EditorTabService
- **WHEN** any caller opens, closes, splits, activates, or saves an editor tab
- **THEN** it SHALL do so through `EditorTabService` and SHALL NOT manipulate tab vectors, active indices, or split trees directly

#### Scenario: Persistence routes through PersistenceService
- **WHEN** workspace state, user configuration, session restore data, or conversation data is read or written
- **THEN** it SHALL route through `PersistenceService`, and no other type SHALL open files in the workspace state directory

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

`PluginHost` SHALL be decomposed into a runtime core plus per-surface extension modules (commands, sidebars, syntax, diagnostics, hover, providers, lifecycle). Each module SHALL own its registry and SHALL NOT exceed 800 lines.

#### Scenario: PluginHost size invariant
- **WHEN** the source tree is built
- **THEN** no single `src/plugin/*.cpp` translation unit SHALL exceed 800 lines, enforced by the architectural-lint test

#### Scenario: Lua VM lifecycle is isolated
- **WHEN** plugin runtime work creates, suspends, or destroys a Lua VM
- **THEN** that work SHALL go through one `LuaRuntime` seam owned by the runtime core; no extension-surface module SHALL hold a raw `lua_State*`

### Requirement: Architectural Invariants Are Enforced By CI

The build SHALL include an architectural-lint test that runs as part of `ctest` and rejects regressions of these invariants without requiring human review.

#### Scenario: Lint is part of the default test run
- **WHEN** `ctest --test-dir build/microide` runs
- **THEN** the architectural-lint test SHALL execute and SHALL fail the run if any banned pattern (new `friend class` in workspace, new `WorkspaceShell&` in coordinator, `try`/`catch` numeric parsing, oversized translation unit) is introduced
