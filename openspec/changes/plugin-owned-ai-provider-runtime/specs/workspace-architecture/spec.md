## MODIFIED Requirements

### Requirement: Service Boundaries For Workspace Subsystems

The workspace SHALL expose a closed set of service interfaces that own their state and define the only mutation API for that state. The minimum service set SHALL include `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PluginRuntimeService`, `AiProviderRuntimeService`, and `PersistenceService`.

#### Scenario: Editor tab mutation routes through EditorTabService
- **WHEN** any caller opens, closes, splits, activates, or saves an editor tab
- **THEN** it SHALL do so through `EditorTabService` and SHALL NOT manipulate tab vectors, active indices, or split trees directly

#### Scenario: Persistence routes through PersistenceService
- **WHEN** workspace state, user configuration, session restore data, or conversation data is read or written
- **THEN** it SHALL route through `PersistenceService`, and no other type SHALL open files in the workspace state directory

#### Scenario: AI provider execution routes through AiProviderRuntimeService
- **WHEN** chat, inline completion, provider auth state, model discovery, or provider cancellation is queried or mutated
- **THEN** it SHALL route through `AiProviderRuntimeService` and SHALL NOT reach into bridge-manager caches, external-agent registries, or shell-private AI state directly

### Requirement: Plugin Host Is Decomposed

`PluginHost` SHALL be decomposed into a runtime core plus per-surface extension modules (commands, sidebars, syntax, diagnostics, hover, auth, AI-provider runtimes, providers, lifecycle). Each module SHALL own its registry or runtime seam and SHALL NOT exceed 800 lines.

#### Scenario: PluginHost size invariant
- **WHEN** the source tree is built
- **THEN** no single `src/plugin/*.cpp` translation unit SHALL exceed 800 lines, enforced by the architectural-lint test

#### Scenario: Lua VM lifecycle is isolated
- **WHEN** plugin runtime work creates, suspends, or destroys a Lua VM
- **THEN** that work SHALL go through one `LuaRuntime` seam owned by the runtime core; no extension-surface module SHALL hold a raw `lua_State*`

#### Scenario: AI provider runtime extension surface is isolated
- **WHEN** plugin-owned AI provider behavior is registered or executed
- **THEN** it SHALL route through the dedicated AI-provider runtime module and SHALL NOT be implemented by ad hoc workspace-side bridge helpers
