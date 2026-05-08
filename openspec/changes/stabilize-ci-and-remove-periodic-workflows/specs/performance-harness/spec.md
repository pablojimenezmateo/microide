## MODIFIED Requirements

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

#### Scenario: Harness workflow trigger policy is non-periodic
- **WHEN** the performance harness workflow is configured in CI
- **THEN** it SHALL run on merge-candidate and manual event-driven triggers, and SHALL NOT define a periodic `schedule` trigger
