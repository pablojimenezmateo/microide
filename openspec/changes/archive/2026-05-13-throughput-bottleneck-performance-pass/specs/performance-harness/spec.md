## ADDED Requirements

### Requirement: Harness Isolates All App State

The performance harness SHALL run scenarios with isolated config, state, cache, and data directories so local user state cannot influence startup, project restore, plugin reload, render, terminal, idle, or allocation metrics. The isolated roots SHALL be created before SDL or workspace initialization and SHALL be cleaned before a normal run starts.

#### Scenario: Local workspace session cannot contaminate scenarios
- **WHEN** `microide_perf --scenarios=cold_startup_no_project --iterations=1` runs on a developer machine that has `~/.local/state/microide/workspace-session`
- **THEN** the scenario SHALL start from the harness-owned empty state root and SHALL NOT restore projects from the developer's real workspace session

#### Scenario: Project session artifacts are isolated
- **WHEN** one perf scenario opens projects and persists project session state
- **THEN** later scenarios in the same harness process SHALL either start from the documented scenario seed state or a clean isolated state, and SHALL NOT inherit unrelated persisted tabs unless the scenario explicitly requests that setup

#### Scenario: Failed run can preserve artifacts for triage
- **WHEN** the harness is invoked with a documented artifact-retention option
- **THEN** it SHALL report the isolated app-root path and preserve that directory after shutdown for debugging

### Requirement: Harness Reports Evidence Provenance

Perf reports SHALL identify whether they were captured from the reference runner or from local advisory runs, and SHALL include enough environment metadata to compare before and after runs.

#### Scenario: Local dummy-driver report is labelled advisory
- **WHEN** a run uses `SDL_VIDEODRIVER=dummy` or a runner that is not `perf-runner-v1`
- **THEN** the JSON and text report SHALL mark the run as local advisory evidence and SHALL NOT be treated as an authoritative baseline update source

#### Scenario: Reference run records runner class
- **WHEN** the harness runs with `--reference-runner=perf-runner-v1`
- **THEN** the report SHALL record the reference runner label, scenario list, iteration count, layout mode, renderer driver, and fixture seed

### Requirement: Before And After Perf Reports Are Paired

Performance changes SHALL keep before and after reports for the same command, scenario set, iteration count, runner class, and SDL environment so reviewers can compare the same workload.

#### Scenario: Optimization closes with paired reports
- **WHEN** a performance optimization task is marked complete
- **THEN** the change record SHALL include the matching before and after `microide_perf` report paths plus any trace paths used to explain the movement

#### Scenario: Baseline update waits for isolated reference evidence
- **WHEN** harness isolation changes local or CI scenario metrics
- **THEN** committed baseline changes SHALL wait for an isolated `perf-runner-v1` run and SHALL include a `perf-baseline:` justification
