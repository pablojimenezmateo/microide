## ADDED Requirements

### Requirement: Architecture Overhaul Preserves Performance Budgets

The architecture overhaul SHALL preserve every existing typing, scrolling, idle CPU, startup, and large-file budget defined elsewhere in this capability. Each service extraction, persistence-format cutover, plugin-host decomposition step, and view-model migration SHALL include before-and-after measurement evidence as part of its change record.

#### Scenario: Service extraction step ships with evidence
- **WHEN** a workspace service (e.g., `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PersistenceService`, `RenderViewModelBuilder`) is extracted from the shell or a coordinator is rewritten against a service interface
- **THEN** the change record SHALL include `MICROIDE_TRACE_REDRAW` output for the surfaces it touches, plus `MICROIDE_STARTUP_TRACE` output if any startup-path code was modified, captured before and after the change

#### Scenario: Persistence format cutover ships with evidence
- **WHEN** the structured persistence format replaces the legacy text-command reader for project state, user configuration, session restore, or conversations
- **THEN** the change record SHALL include startup trace output showing the load step's wall time before and after, on a representative project, and SHALL show the new step is no slower than the previous text-format load

#### Scenario: Plugin host decomposition ships with evidence
- **WHEN** `PluginHost` is decomposed into the runtime core and per-surface modules, or any extracted module is rewritten
- **THEN** the change record SHALL include startup trace output covering plugin load and the first idle frame, before and after, with no regression beyond the documented startup budget

#### Scenario: View model migration ships with evidence
- **WHEN** a render surface is migrated to consume a view-model struct
- **THEN** the change record SHALL include `MICROIDE_TRACE_REDRAW` output for that surface, before and after, and frame time SHALL not regress
