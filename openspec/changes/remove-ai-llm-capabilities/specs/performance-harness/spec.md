## MODIFIED Requirements

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
