# performance-harness Specification

## Purpose
TBD - created by archiving change comprehensive-tech-debt-and-perf-harness. Update Purpose after archive.
## Requirements
### Requirement: Headless-But-Real Performance Harness Binary

The repository SHALL ship a `microide_perf` binary that drives a real SDL window through scripted scenarios on the `SDL_RENDERER_SOFTWARE` backend with a fixed window size, fixed DPI, and the bundled debug font, and SHALL NOT depend on `SDL_VIDEODRIVER=dummy` for measurement.

#### Scenario: Harness uses the software renderer
- **WHEN** the harness binary starts a scenario
- **THEN** it SHALL initialize SDL with the software renderer, paint frames into a memory surface, and exercise the same render-path code (clip rects, retained-redraw promotion, view-model construction) that the production binary uses

#### Scenario: Harness window configuration is fixed
- **WHEN** any scenario runs
- **THEN** the SDL window SHALL be created at 1920x1080 with DPI 1.0 and the bundled debug font, and SHALL NOT vary based on the host display configuration

#### Scenario: Harness is registered as a CTest entry
- **WHEN** `ctest --test-dir build` runs
- **THEN** the harness SHALL be invokable as the `microide_perf_tests` entry, and the entry SHALL fail the run if any scenario regresses beyond the per-metric tolerance against its committed baseline

### Requirement: Scenario Definition And Coverage

Performance scenarios SHALL be authored as C++ files under `tests/perf/scenarios/<name>.cpp` that register themselves with a static registrar and declare their setup, driving loop, and per-iteration assertions inline. The repository SHALL ship at minimum the following scenarios as part of the initial harness landing.

#### Scenario: Initial scenario set covers required workloads
- **WHEN** the harness suite runs
- **THEN** it SHALL include scenarios named `cold_startup_no_project`, `cold_startup_small_project`, `cold_startup_large_project`, `multi_project_switch`, `multi_tab_cycle`, `typing_small_file`, `typing_large_file`, `scroll_large_file`, `project_search_literal`, `project_search_regex`, `linter_on_save`, `compare_tab_open`, `merge_tab_open`, `terminal_scroll_long_output`, and `idle_soak_30s`

#### Scenario: New features ship with a perf scenario
- **WHEN** a change adds a new user-facing hot path (editor surface, sidebar surface, overlay, render path, background-task category)
- **THEN** the change SHALL add at least one perf scenario covering that hot path, with a committed baseline, in the same change

#### Scenario: Scenarios are deterministic
- **WHEN** any scenario runs
- **THEN** it SHALL use a fixed random seed, fixed fixture project trees committed under `tests/perf/fixtures/`, plugins disabled by default (opt-in per scenario), and frame ticks driven by explicit `PumpFrames(N)` calls rather than wall-clock scheduling

### Requirement: Structured Metric Capture

Every scenario SHALL capture a documented metric set per iteration and SHALL run a configurable number of iterations (N≥10 by default), reporting the median across iterations as the comparison value while preserving every percentile in the baseline.

#### Scenario: Per-frame metrics are captured
- **WHEN** a scenario completes
- **THEN** the harness SHALL produce, for each scenario, the per-frame render time (p50, p90, p95, p99, max), the full-redraw count, the partial-redraw count, the promote-to-full count, the per-frame allocation count, the total wall time, the user+sys CPU time, the start and end RSS, the SDL wake-up count, and the aggregate background-task count

#### Scenario: Allocation counter is exact
- **WHEN** the harness build flag `MICROIDE_PERF_HARNESS_BUILD` is enabled
- **THEN** the harness SHALL instrument global `operator new` and `operator delete` to expose `Allocations::Snapshot()`, and render-path scenarios SHALL be able to assert `AssertNoAllocationsDuringDraw()` to guard against silent regressions

#### Scenario: Median over N iterations is the comparison value
- **WHEN** a scenario produces metrics across N iterations
- **THEN** the harness SHALL compare the median value per metric against the baseline; mean values SHALL NOT be used because tail-latency regressions can hide behind a stable mean

### Requirement: Committed Baselines And CI Regression Gate

Performance baselines SHALL be committed JSON files under `tests/perf/baselines/<scenario>.json`, one file per scenario, and a CI runner labeled `perf-runner-v1` SHALL run the harness on every merge candidate and fail the merge on any regression beyond the per-metric tolerance.

#### Scenario: Baselines are visible in the PR diff
- **WHEN** a change moves a baseline
- **THEN** the modified `tests/perf/baselines/*.json` file SHALL be part of the same commit, SHALL be reviewable in the PR diff, and SHALL NOT live in an external store

#### Scenario: CI gate exits with a documented status
- **WHEN** the harness runs on `perf-runner-v1`
- **THEN** it SHALL exit 0 on no regression, 1 on regression beyond tolerance, and 2 on harness error; the merge gate SHALL block on exit code 1 and SHALL NOT block on exit code 0 or 2

#### Scenario: Baseline movement requires a tagged change record
- **WHEN** a `tests/perf/baselines/*.json` file is modified in a commit
- **THEN** the commit message or PR description SHALL include a line beginning with `perf-baseline:` explaining the move, and a pre-merge check SHALL fail otherwise

#### Scenario: Smoke subset for fork PRs
- **WHEN** a CI run executes on a runner not labeled `perf-runner-v1`
- **THEN** the harness SHALL run a smoke-only subset of scenarios, SHALL emit advisory output only, and SHALL NOT gate the merge

### Requirement: Per-Metric Tolerance Configuration

Each baseline file SHALL carry per-metric tolerance windows. Default tolerances SHALL be p50 ±10 %, p95 ±20 %, and max ±50 %, and a scenario MAY override them with documented justification in the baseline file.

#### Scenario: Default tolerances apply when not overridden
- **WHEN** a baseline file does not specify a tolerance for a metric
- **THEN** the harness SHALL apply the documented default tolerances for that metric

#### Scenario: Overrides require justification
- **WHEN** a baseline file overrides a default tolerance
- **THEN** the file SHALL include a `rationale` string explaining why, and the rationale SHALL be reviewable in the PR diff

### Requirement: Harness Acts As The Regression Oracle For Internal Refactors

Service extractions, render-path edits, persistence-format changes, plugin-host changes, and view-model migrations SHALL use the harness as the primary regression oracle. The change record SHALL cite the harness CI run, not a hand-captured trace.

#### Scenario: Service extraction cites the harness run
- **WHEN** a workspace service is extracted from the shell, a coordinator is rewritten, or a render surface is migrated to a new view model
- **THEN** the change record SHALL cite the harness CI run as the regression oracle, and SHALL only include hand-captured `MICROIDE_STARTUP_TRACE` or `MICROIDE_PERF_TRACE` output as a debugging fallback when the harness reports a regression

#### Scenario: Hand-captured traces remain available
- **WHEN** a developer needs to investigate a regression flagged by the harness
- **THEN** the existing `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, and `MICROIDE_TRACE_REDRAW` env-variable surfaces SHALL continue to function and produce the same output they do today

### Requirement: Hotspot Coverage Expansion Is Mandatory
The performance harness SHALL maintain explicit scenario coverage for any hotspot class identified by an approved hotspot-audit pass. New hotspot classes SHALL NOT remain untracked after the pass completes.

#### Scenario: Hotspot class lacks scenario coverage
- **WHEN** the hotspot-audit ledger includes a class not represented by current scenarios
- **THEN** the change SHALL add at least one deterministic `tests/perf/scenarios/<name>.cpp` entry and a matching `tests/perf/baselines/<name>.json` baseline in the same change

### Requirement: Hotspot Scenarios Capture Multi-Metric Evidence
Scenarios introduced for hotspot coverage SHALL capture frame-time percentiles, wall time, CPU time, redraw counts, wake-up counts, allocation counts, and RSS movement so regression triage can distinguish rendering, scheduling, and memory causes.

#### Scenario: New hotspot scenario reports full metric set
- **WHEN** a hotspot-protection scenario is executed
- **THEN** its baseline and output SHALL include all standard harness metrics required by the structured metric capture contract and SHALL be reviewable in pull request diffs

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

