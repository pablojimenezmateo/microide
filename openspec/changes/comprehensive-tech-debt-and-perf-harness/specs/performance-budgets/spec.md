## ADDED Requirements

### Requirement: Scenario-Level Budgets For Multi-Project, Multi-Tab, Linter, And Idle Workloads

The performance harness SHALL gate merges on scenario-level budgets that cover workloads previously not measured at all: project switching, tab cycling, linter-on-save (plugin diagnostics), and idle wake-ups. Each scenario in `tests/perf/baselines/` SHALL act as a budget for its workload.

#### Scenario: Project-switch budget
- **WHEN** the `multi_project_switch` scenario runs (open five projects, switch among them)
- **THEN** the harness SHALL fail the merge if median project-switch wall time, frame-time percentiles, or RSS growth regress beyond the per-metric tolerance against the committed baseline

#### Scenario: Tab-cycle budget
- **WHEN** the `multi_tab_cycle` scenario runs (open twenty file tabs in one project, cycle through them)
- **THEN** the harness SHALL fail the merge if median tab-cycle wall time or frame-time percentiles regress beyond tolerance

#### Scenario: Linter-on-save budget
- **WHEN** the `linter_on_save` scenario runs (save triggers a plugin diagnostics provider)
- **THEN** the harness SHALL fail the merge if save-to-diagnostics-published wall time regresses beyond tolerance

#### Scenario: Idle wake-up budget
- **WHEN** the `idle_soak_30s` scenario runs and the bug-detection nightly long-soak runs
- **THEN** the harness SHALL fail on SDL wake-up count exceeding a documented per-second (idle_soak_30s) and per-hour (long_soak_8h) budget

## MODIFIED Requirements

### Requirement: Measured-Before-Merged Policy

Any change that modifies a hot render, text-layout, retained-scene, compare, merge, terminal, AI-hot path, file-watching path, or event-loop idle path SHALL be gated by the automated performance harness. The harness CI run on the `perf-runner-v1` reference machine SHALL act as the merge gate; hand-captured `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, `MICROIDE_TRACE_REDRAW`, `microide_diff_bench`, and `microide_search_bench` output SHALL remain available as the developer fallback when the harness reports a regression. Review SHALL NOT accept code-inspection-only claims of performance impact.

#### Scenario: Reviewer asked to accept an unmeasured perf claim
- **WHEN** a change claims a performance win or denies a performance risk on a hot path
- **THEN** the reviewer SHALL require either a green harness run on `perf-runner-v1` (no regression beyond tolerance) or, when the harness flags movement, a corresponding baseline update tagged with `perf-baseline:` plus a justification in the change record

#### Scenario: Idle regression risk
- **WHEN** a change touches project watchers, plugin watchers, scheduled wake routing, or background task polling
- **THEN** the change SHALL produce a green run for the `idle_soak_30s` harness scenario, and the long-soak nightly run SHALL also be green on the next nightly cycle before the change is considered settled

#### Scenario: Harness flags a regression and the author confirms it
- **WHEN** the harness reports a regression beyond tolerance and the change author intends the move
- **THEN** the author SHALL update the affected `tests/perf/baselines/<scenario>.json` files in the same change, SHALL include a `perf-baseline:` line in the change record explaining the move, and the pre-merge baseline-tag check SHALL pass

### Requirement: Architecture Overhaul Preserves Performance Budgets

The architecture overhaul SHALL preserve every existing typing, scrolling, idle CPU, startup, and large-file budget defined elsewhere in this capability. Each service extraction, persistence-format cutover, plugin-host decomposition step, and view-model migration SHALL use the automated harness as the regression oracle and SHALL cite the harness CI run in the change record.

#### Scenario: Service extraction step ships with harness evidence
- **WHEN** a workspace service (e.g., `EditorTabService`, `ProjectCatalogService`, `PromptSurfaceService`, `SidebarService`, `CompareMergeService`, `TerminalPanelService`, `PersistenceService`, `RenderViewModelBuilder`) is extracted from the shell or a coordinator is rewritten against a service interface
- **THEN** the change record SHALL cite a green harness CI run covering the surfaces it touches; hand-captured trace output SHALL only appear if the harness flagged a regression that needed deeper investigation

#### Scenario: Persistence format cutover ships with harness evidence
- **WHEN** the structured persistence format replaces the legacy text-command reader for project state, user configuration, session restore, or conversations, or when the legacy importer is later removed
- **THEN** the harness `cold_startup_*` scenarios SHALL pass without regression, and the change record SHALL cite that run

#### Scenario: Plugin host decomposition ships with harness evidence
- **WHEN** `PluginHost` is decomposed into the runtime core and per-surface modules, or any extracted module is rewritten
- **THEN** the harness `cold_startup_small_project`, `cold_startup_large_project`, and `linter_on_save` scenarios SHALL pass without regression

#### Scenario: View model migration ships with harness evidence
- **WHEN** a render surface is migrated to consume a view-model struct
- **THEN** the harness scenarios that exercise that surface (typing, scrolling, project switch, tab cycling, compare or merge, chat pane, as applicable) SHALL pass without regression
