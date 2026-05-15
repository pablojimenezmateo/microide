## MODIFIED Requirements

### Requirement: Scenario Definition And Coverage

Performance scenarios SHALL be authored as C++ files under `tests/perf/scenarios/<name>.cpp` that register themselves with a static registrar and declare their setup, driving loop, and per-iteration assertions inline. The repository SHALL ship at minimum the following scenarios as part of the initial harness landing, plus the `editor_scroll_only_no_content_bump` scenario added by the tiered-document-revisions change.

#### Scenario: Initial scenario set covers required workloads
- **WHEN** the harness suite runs
- **THEN** it SHALL include scenarios named `cold_startup_no_project`, `cold_startup_small_project`, `cold_startup_large_project`, `multi_project_switch`, `multi_tab_cycle`, `typing_small_file`, `typing_large_file`, `scroll_large_file`, `project_search_literal`, `project_search_regex`, `linter_on_save`, `compare_tab_open`, `merge_tab_open`, `terminal_scroll_long_output`, `idle_soak_30s`, and `editor_scroll_only_no_content_bump`

#### Scenario: New features ship with a perf scenario
- **WHEN** a change adds a new user-facing hot path (editor surface, sidebar surface, overlay, render path, background-task category)
- **THEN** the change SHALL add at least one perf scenario covering that hot path, with a committed baseline, in the same change

#### Scenario: Scenarios are deterministic
- **WHEN** any scenario runs
- **THEN** it SHALL use a fixed random seed, fixed fixture project trees committed under `tests/perf/fixtures/`, plugins disabled by default (opt-in per scenario), and frame ticks driven by explicit `PumpFrames(N)` calls rather than wall-clock scheduling

#### Scenario: Scroll-only fixture asserts tier isolation
- **WHEN** the `editor_scroll_only_no_content_bump` scenario runs (open a large syntax-highlighted file, warm caches, then scroll N frames without typing or theme changes)
- **THEN** the harness SHALL report `editor.content_revision_bumps == 0`, `editor.syntax_revision_bumps == 0`, and `editor.layout_shape_revision_bumps == 0` over the measurement window; `editor.presentation_revision_bumps` MAY be non-zero

## ADDED Requirements

### Requirement: Tier-Bump Counters Are Reportable

The harness SHALL include `editor.content_revision_bumps`, `editor.syntax_revision_bumps`, `editor.layout_shape_revision_bumps`, and `editor.presentation_revision_bumps` in the structured metric capture for every editor-touching scenario, both as raw counts and as part of the JSON `perf_counters` block.

#### Scenario: Tier counters appear in scenario JSON
- **WHEN** any scenario that exercises the editor produces a `--report-json` payload
- **THEN** the four `editor.*_revision_bumps` counters SHALL appear under `perf_counters` with a non-negative integer value each

#### Scenario: Tier counter regression is gateable
- **WHEN** the `editor_scroll_only_no_content_bump` scenario reports a non-zero `editor.content_revision_bumps`, `editor.syntax_revision_bumps`, or `editor.layout_shape_revision_bumps`
- **THEN** the harness CI gate SHALL fail the merge, treating this as a regression beyond tolerance independent of wall-time movement
