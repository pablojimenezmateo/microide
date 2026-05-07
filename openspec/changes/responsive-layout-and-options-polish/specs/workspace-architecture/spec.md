## MODIFIED Requirements

### Requirement: Service Boundaries For Workspace Subsystems

The workspace SHALL expose a closed set of service interfaces that own their state and define the only mutation API for that state. The minimum service set SHALL include `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `AiProviderRuntimeService`, `PersistenceService`, `LayoutModeService`, `StatusBarService`, and `SettingsOverlayService`.

#### Scenario: Editor tab mutation routes through EditorTabService
- **WHEN** any caller opens, closes, splits, activates, or saves an editor tab
- **THEN** it SHALL do so through `EditorTabService` and SHALL NOT manipulate tab vectors, active indices, or split trees directly

#### Scenario: Persistence routes through PersistenceService
- **WHEN** workspace state, user configuration, session restore data, or conversation data is read or written
- **THEN** it SHALL route through `PersistenceService`, and no other type SHALL open files in the workspace state directory

#### Scenario: AI provider execution routes through AiProviderRuntimeService
- **WHEN** chat, inline completion, provider auth state, model discovery, or provider cancellation is queried or mutated
- **THEN** it SHALL route through `AiProviderRuntimeService` and SHALL NOT reach into bridge-manager caches, external-agent registries, or shell-private AI state directly

#### Scenario: Layout mode is a single service
- **WHEN** any caller reads or writes the active `LayoutMode`
- **THEN** it SHALL do so through `LayoutModeService`, and no caller other than `ComputeLayout` SHALL recompute the breakpoint independently

#### Scenario: Status bar mutation routes through StatusBarService
- **WHEN** any caller updates a status-bar segment value (project label, branch, line/column, problems count, LSP state, AI provider/model, encoding, indent display, language)
- **THEN** it SHALL do so through `StatusBarService`, and the status-bar render TU SHALL read only the view model produced from that service

#### Scenario: Settings overlay mutation routes through SettingsOverlayService
- **WHEN** the settings overlay opens, filters, edits, or persists a setting
- **THEN** it SHALL do so through `SettingsOverlayService`, which SHALL be the only consumer of the overlay's mutable state, and the new render TU SHALL consume only its view model

## ADDED Requirements

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
