## Why

The current performance harness protects known regressions, but it does not yet provide a thorough hotspot map across startup, editing, terminal, search, and shell rendering paths. A focused pass is needed now to identify and prioritize the highest-impact CPU, latency, and memory opportunities before additional feature work compounds cost.

## What Changes

- Run a repository-wide performance hotspot audit across critical user flows (startup, typing, scrolling, resize, project search, terminal output, and plugin-host interactions).
- Expand perf scenario coverage where gaps prevent reliable detection of regressions in hot paths.
- Define a repeatable triage workflow that converts observed hotspots into ranked optimization tasks with before/after evidence.
- Tighten performance gate expectations so newly discovered hotspot categories remain protected by automated checks.

## Capabilities

### New Capabilities
- `performance-hotspot-audit`: Standardize hotspot discovery, ranking, and evidence capture for CPU time, frame cost, memory churn, and latency spikes across core IDE workflows.

### Modified Capabilities
- `performance-harness`: Add or refine perf scenarios and baselines to cover newly identified hotspot-prone workflows.
- `performance-budgets`: Extend budget requirements to include newly measured critical-path metrics discovered by the hotspot audit.
- `workspace-architecture`: Clarify durable ownership and execution constraints for hot paths uncovered by the audit (for example, avoiding expensive shell-thread work).

## Impact

- Affected systems: `tests/perf/*`, `docs/perf-harness.md`, performance baseline fixtures, and workspace/render/service code touched by prioritized optimizations.
- Product impact: lower latency and CPU usage in common workflows, with stronger regression detection before merge.
- Process impact: introduces a repeatable throughput-oriented performance audit loop that can be reused for future passes.
