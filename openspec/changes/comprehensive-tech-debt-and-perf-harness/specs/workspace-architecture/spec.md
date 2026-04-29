## ADDED Requirements

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
