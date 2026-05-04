## ADDED Requirements

### Requirement: New Harness Scenarios For Background-Work And Idle Paths

The performance harness SHALL include scenarios covering the file-finder cold-open, git sidebar activation, search time-to-first-result, and idle CPU settling paths introduced by this change. Each scenario SHALL have a committed baseline and SHALL be part of the CI regression gate.

#### Scenario: file_finder_cold scenario is registered and passes
- **WHEN** the harness runs the `file_finder_cold` scenario
- **THEN** it SHALL open a project with a fully built in-process file index, simulate the file-finder overlay open action, and assert that the time from open to first result render is within the budget defined in `performance-budgets`

#### Scenario: git_sidebar_activate scenario is registered and passes
- **WHEN** the harness runs the `git_sidebar_activate` scenario
- **THEN** it SHALL open a project backed by a git repository, activate the git sidebar, and assert that the time from activation to first rendered git-status data is within the budget defined in `performance-budgets`

#### Scenario: search_first_result scenario is registered and passes
- **WHEN** the harness runs the `search_first_result` scenario
- **THEN** it SHALL initiate a project search on the 10 000-file fixture, measure the elapsed time from search initiation to the first result batch visible in the UI, and assert the result is within the budget defined in `performance-budgets`

#### Scenario: idle_soak_30s scenario confirms near-zero CPU for new services
- **WHEN** the harness runs the `idle_soak_30s` scenario after this change
- **THEN** it SHALL additionally verify that the file-index watcher thread and the git executor thread are both parked (zero wake events generated) for the full 30-second soak period after their startup work completes

### Requirement: New Scenarios Ship With Committed Baselines

Each new scenario (`file_finder_cold`, `git_sidebar_activate`, `search_first_result`, and the extended `idle_soak_30s`) SHALL have a corresponding committed baseline JSON file under `tests/perf/baselines/` in the same change that introduces the scenario. The baselines SHALL be captured on the `perf-runner-v1` reference machine.

#### Scenario: New baseline files are present at merge time
- **WHEN** the change that introduces the new harness scenarios is merged
- **THEN** `tests/perf/baselines/file_finder_cold.json`, `tests/perf/baselines/git_sidebar_activate.json`, and `tests/perf/baselines/search_first_result.json` SHALL be present and SHALL have been captured on the reference host, with the `perf-baseline:` tag in the change record
