## Purpose

Define measurable startup, frame-time, idle-CPU, and background-work performance budgets for MicroIDE, and require evidence for hot-path changes.
## Requirements
### Requirement: Startup Budget

MicroIDE SHALL complete cold startup (binary launch to interactive shell with the last-session project restored) within a documented budget on a reference Linux host, measured through `docs/startup-tracing.md`. Changes that affect startup paths or schedule startup-triggered background work SHALL record startup trace output and startup-to-idle evidence in the change record.

#### Scenario: Startup regression
- **WHEN** a change modifies `src/app/*` bootstrap, workspace-session restore, plugin runtime startup, syntax snapshot loading, project file-watching startup, or startup-triggered refresh scheduling
- **THEN** the change record SHALL include `MICROIDE_STARTUP_TRACE` output before and after the change plus corresponding evidence that the application settles to near-zero idle CPU after startup completes

#### Scenario: Last-session restore correctness
- **WHEN** the previous session held open editor tabs, compare tabs, merge tabs, and terminals
- **THEN** cold startup SHALL restore each of those surfaces to its recorded state as part of the budgeted startup path, not as a deferred post-startup task

### Requirement: Typing And Scrolling Frame Budget

Editor typing, editor scrolling, compare scrolling, merge scrolling, terminal scrolling, soft-wrap relayout, and multiple-caret edits SHALL fit within a single-digit millisecond frame budget on the reference host. Changes that modify text layout, wrapped-line mapping, selection fan-out, syntax state, retained-scene redraw policy, viewport scroll math, or editor mutation plumbing SHALL include `MICROIDE_TRACE_REDRAW` before-and-after output.

Single-range editor mutations on large files — including insert, delete, backspace, paste, completion acceptance, undo, and redo — SHALL avoid whole-buffer snapshotting and full-text synchronization on the main input path when the active LSP client supports incremental sync.

#### Scenario: Typing into a large open file
- **WHEN** the editor has a file loaded that triggers the large-file code path and the user holds a printable key
- **THEN** each insertion SHALL render within the frame budget, SHALL NOT produce a full-surface repaint per keystroke, and SHALL NOT regress measurable typing latency compared to the prior release

#### Scenario: Typing with soft wrap and multiple carets
- **WHEN** soft wrap is enabled and the user edits through multiple carets in a long file
- **THEN** wrapped-line recompute, caret placement, and repaint SHALL remain within the frame budget without degrading unrelated editor responsiveness

#### Scenario: Scrolling a merge tab
- **WHEN** a three-way merge tab is scrolled with the mouse wheel or `PageDown`
- **THEN** each repaint SHALL fit within the frame budget and SHALL reuse the shared row-decoration cache rather than rebuilding decorations per frame

#### Scenario: Delete and undo in a large LSP-backed file
- **WHEN** the user presses Delete, Backspace, or `Ctrl+Z` in a large open file whose active language server supports incremental sync
- **THEN** the input handler SHALL stay off the full-buffer snapshot and full-text LSP path, the redraw invalidation SHALL be derived from the applied edit rather than a whole-buffer diff, and the edit SHALL remain interactive without multi-hundred-millisecond stalls

### Requirement: Idle CPU Budget

When MicroIDE is loaded with a project but has no active user input, no running terminal output, and no in-flight background work, the application SHALL consume near-zero CPU and SHALL NOT produce unnecessary SDL wake events, including immediately after startup-triggered watcher or refresh activity has settled.

#### Scenario: Idle application
- **WHEN** MicroIDE has a project open, the user has not interacted for 30 seconds, and no background task is active
- **THEN** `top` or equivalent SHALL report effectively no CPU time attributed to the process, and the event loop SHALL be parked on an SDL wait rather than polling or repeated zero-delay timeout wakes

#### Scenario: Startup-triggered watcher activity
- **WHEN** project file watching, plugin asset watching, or other startup-triggered background refresh work is armed
- **THEN** each service SHALL either block on a native wake source or wait for a real poll deadline, and SHALL NOT keep the application in a zero-delay wake loop when no user-visible work is pending

### Requirement: Background Work Isolation

Git, search, blame, LSP, DAP, formatter, and plugin background work SHALL NOT starve one another and SHALL NOT stall the render or input hot paths. Cancellation SHALL flow across project switch, tab close, and shutdown.

#### Scenario: Long-running search during git refresh
- **WHEN** the user triggers a git refresh while a project search is running
- **THEN** both tasks SHALL progress concurrently, and MicroIDE SHALL continue to accept editor input at the frame budget

#### Scenario: Project switch cancels in-flight work
- **WHEN** the user switches projects while LSP requests, blame requests, and search are in flight
- **THEN** each in-flight request SHALL be cancelled or ignored on completion, and the new project SHALL start with a clean set of background tasks

### Requirement: Measured-Before-Merged Policy

Any change that modifies a hot render, text-layout, retained-scene, compare, merge, terminal, file-watching path, or event-loop idle path SHALL be gated by the automated performance harness. The harness CI run on the `perf-runner-v1` reference machine SHALL act as the merge gate; hand-captured `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, `MICROIDE_TRACE_REDRAW`, `microide_diff_bench`, and `microide_search_bench` output SHALL remain available as the developer fallback when the harness reports a regression. Review SHALL NOT accept code-inspection-only claims of performance impact.

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
- **WHEN** the structured persistence format replaces the legacy text-command reader for project state, user configuration, or session restore, or when the legacy importer is later removed
- **THEN** the harness `cold_startup_*` scenarios SHALL pass without regression, and the change record SHALL cite that run

#### Scenario: Plugin host decomposition ships with harness evidence
- **WHEN** `PluginHost` is decomposed into the runtime core and per-surface modules, or any extracted module is rewritten
- **THEN** the harness `cold_startup_small_project`, `cold_startup_large_project`, and `linter_on_save` scenarios SHALL pass without regression

#### Scenario: View model migration ships with harness evidence
- **WHEN** a render surface is migrated to consume a view-model struct
- **THEN** the harness scenarios that exercise that surface (typing, scrolling, project switch, tab cycling, compare or merge, as applicable) SHALL pass without regression

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

### Requirement: Ignored-Tree Visibility And Expansion Stay Interactive

Project-tree initial load SHALL surface ignored nodes without recursively walking ignored descendants, and expanding an ignored directory SHALL enumerate only the requested level within a documented interactive latency budget. Changes that modify ignored-node discovery or expansion SHALL include harness or trace evidence showing that initial tree open does not scan the full ignored subtree.

#### Scenario: Initial tree open with a large ignored directory
- **WHEN** a project contains a large ignored directory such as `node_modules/`
- **THEN** the initial project tree SHALL surface the ignored directory node without recursively enumerating its full descendant set

#### Scenario: Expanding an ignored directory enumerates one level
- **WHEN** the user expands an ignored directory in the project tree
- **THEN** the UI SHALL enumerate only that directory's immediate children within an interactive latency budget instead of scanning the entire ignored subtree

### Requirement: Hotspot Opportunity Ledger Is Budget-Aware
A hotspot-audit change SHALL map each high-priority finding to one or more existing or newly added budgeted scenarios, and SHALL define acceptance criteria in terms of measurable budget movement.

#### Scenario: Ranked hotspot has no budget mapping
- **WHEN** an optimization candidate is marked high priority
- **THEN** the change SHALL document which scenario metrics define success and SHALL add or update a budgeted scenario when no suitable mapping exists

### Requirement: Throughput-Oriented Performance Pass Is Measured Before Merge
Changes produced by a hotspot pass SHALL include measured-before-merge evidence for both targeted and adjacent hot paths to prevent regressions hidden by local improvements.

#### Scenario: Optimization improves one path but risks another
- **WHEN** a hotspot optimization modifies shared infrastructure used by multiple interactive workflows
- **THEN** the author SHALL run and cite harness scenarios for the targeted workflow and at least one adjacent workflow that shares the modified path
