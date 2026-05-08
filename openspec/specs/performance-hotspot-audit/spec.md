# performance-hotspot-audit Specification

## Purpose
TBD - created by syncing change performance-hotspot-throughput-pass. Update Purpose after archive.

## Requirements
### Requirement: Repository-Wide Hotspot Audit Loop
MicroIDE SHALL define and execute a repeatable hotspot-audit loop that inspects startup, typing, scrolling, search, terminal output, plugin-triggered updates, and idle behavior, and SHALL produce evidence-backed findings rather than intuition-only claims.

#### Scenario: Audit pass executes on core workflows
- **WHEN** a performance hotspot pass is initiated
- **THEN** the pass SHALL run deterministic measurements for startup, editor typing, editor scrolling, compare or merge scrolling, project search, terminal long-output scrolling, and idle-soak behavior using existing harness scenarios plus newly added scenarios where coverage is missing

### Requirement: Hotspot Findings Are Ranked And Actionable
The hotspot pass SHALL output a ranked opportunity ledger where each entry includes affected subsystem, observed metric movement, expected user impact, implementation complexity, and verification steps.

#### Scenario: Opportunity ranking uses measurable evidence
- **WHEN** hotspot data is reviewed for planning implementation work
- **THEN** each candidate optimization SHALL include before/after metric context from the harness or trace evidence and SHALL be prioritized by estimated impact-to-effort ratio

### Requirement: Audit Findings Close Harness Gaps
If a hotspot category is discovered without stable automated coverage, the same pass SHALL add or update at least one deterministic harness scenario and baseline that protects the category against regressions.

#### Scenario: Uncovered hotspot becomes a harness scenario
- **WHEN** the audit identifies a recurring regression risk that is not represented in `tests/perf/scenarios/`
- **THEN** the change SHALL add a scenario and committed baseline for that risk before the audit pass is considered complete
