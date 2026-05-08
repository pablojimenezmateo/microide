## MODIFIED Requirements

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

### Requirement: Plugin Host Is Decomposed

`PluginHost` SHALL be decomposed into a runtime core plus per-surface extension modules (commands, sidebars, syntax, diagnostics, hover, auth, providers, lifecycle). Each module SHALL own its registry or runtime seam and SHALL NOT exceed 800 lines.

#### Scenario: PluginHost size invariant
- **WHEN** the source tree is built
- **THEN** no single `src/plugin/*.cpp` translation unit SHALL exceed 800 lines, enforced by the architectural-lint test

#### Scenario: Lua VM lifecycle is isolated
- **WHEN** plugin runtime work creates, suspends, or destroys a Lua VM
- **THEN** that work SHALL go through one `LuaRuntime` seam owned by the runtime core; no extension-surface module SHALL hold a raw `lua_State*`

## REMOVED Requirements

### Requirement: AI provider runtime extension surface is isolated
**Reason**: Plugin AI-provider runtime extension surface is retired with AI/LLM feature removal.
**Migration**: Remove AI-provider runtime extension modules and workspace-side execution paths tied to those modules.
