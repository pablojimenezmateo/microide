## ADDED Requirements

### Requirement: Cleanup Cutover Ships With Harness Evidence

The `codebase-cleanup-perf-and-debt` change SHALL be gated by a green run of the performance harness across `cold_startup_small_project`, `cold_startup_large_project`, `typing_steady_state`, `idle_soak_30s`, and `linter_on_save`. The change record SHALL cite the green run, and any baseline movement SHALL be recorded in `tests/perf/baselines/<scenario>.json` with a `perf-baseline:` justification line in the change record.

#### Scenario: Legacy-importer deletion ships with cold-startup evidence
- **WHEN** the commit deleting `WorkspacePersistenceLegacyFormat.{h,cpp}` lands
- **THEN** `cold_startup_small_project` and `cold_startup_large_project` SHALL be green on the same release; baseline movement SHALL be justified

#### Scenario: Formatter-async migration ships with linter-on-save evidence
- **WHEN** the formatter call is moved from synchronous `platform::RunSubprocess` to `ProjectBackgroundExecutor`
- **THEN** `linter_on_save` SHALL be green; if p99 moves outside tolerance the author SHALL update the baseline in the same commit with a `perf-baseline:` line

#### Scenario: Render-allocation removal ships with typing/idle evidence
- **WHEN** per-frame `std::string` materialization is removed from `WorkspaceShellRenderSidebar.cpp` and replaced with view-model fields
- **THEN** `typing_steady_state` and `idle_soak_30s` SHALL be green on the same release
