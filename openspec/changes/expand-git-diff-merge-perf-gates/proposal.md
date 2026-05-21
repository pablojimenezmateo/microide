## Why

The performance harness already covers important compare/merge/git paths, but the roadmap identifies gaps exactly where the workstation wedge will grow: large hunk navigation, mixed edit/scroll traces, large merge-model builds, hunk/line staging, and external-change refresh. These need budgeted scenarios before the UI expands further.

## What Changes

- Add deterministic performance scenarios for large repository Git refresh, many untracked files, large diff open/navigation, hunk and selected-line staging, many-conflict merge open/navigation, merge accept/edit/scroll, large staged commit workflow, and external refresh of open diff/merge tabs.
- Extend performance budget requirements to treat Git/diff/merge workstation interactions as gated hot paths.
- Ensure each scenario reports standard wall, frame, CPU, allocation, RSS, wake-up, redraw, and background-task metrics with committed baselines.

## Capabilities

### New Capabilities

### Modified Capabilities
- `performance-harness`: Adds required Git/diff/merge workstation scenarios and fixture expectations.
- `performance-budgets`: Adds user-centered budgets for Git sidebar refresh, diff navigation/staging, merge interactions, commit open, and external refresh paths.

## Impact

- Affects `tests/perf/scenarios/`, `tests/perf/fixtures/`, `tests/perf/baselines/`, harness docs, and CI baseline policy.
- Should be implemented before or alongside the interactive features it gates.
