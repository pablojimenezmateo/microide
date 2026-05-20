# performance-hotspot-audit Specification

## Purpose
Define the repeatable hotspot-audit loop used to turn performance suspicions into ranked,
evidence-backed work. Audits must pair measurements with implementation tasks, identify harness
coverage gaps, separate measurement defects from product bottlenecks, and record reproducible
commands for before/after validation.

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

### Requirement: Hotspot Ledger Includes Before And After Evidence

Each hotspot audit SHALL produce a ledger that ranks findings by measured impact, confidence, user-visible importance, implementation cost, and verification coverage. The ledger SHALL include raw before and after report paths when implementation is complete.

#### Scenario: Finding cites raw evidence
- **WHEN** a finding is ranked high priority
- **THEN** the ledger SHALL cite the harness report or trace file that exposed it, the scenario metrics involved, and the affected subsystem

#### Scenario: After evidence is required before completion
- **WHEN** implementation work for a hotspot finding is complete
- **THEN** the ledger SHALL include after-run report paths for the same scenario set and SHALL state whether the movement met, exceeded, or missed the target

### Requirement: Audit Separates Measurement Noise From Product Bottlenecks

The audit SHALL distinguish harness or environment defects from product-runtime bottlenecks so optimization tasks do not tune the wrong subsystem.

#### Scenario: Harness contamination is ranked separately
- **WHEN** a scenario includes work caused by external user state, non-isolated app directories, missing fixtures, or a non-reference SDL driver
- **THEN** the audit SHALL rank that as a measurement defect, mark affected metrics as advisory, and define the harness fix needed before authoritative after numbers are accepted

#### Scenario: Product bottleneck remains after measurement fix
- **WHEN** a hotspot remains visible after harness isolation or environment correction
- **THEN** the audit SHALL rank it as a product bottleneck and map it to implementation tasks and budgeted scenarios

### Requirement: Audit Commands Are Reproducible

The audit record SHALL include exact commands for the local before run, the local after run, and the reference-runner gate run.

#### Scenario: Developer reruns the audit locally
- **WHEN** a developer follows the commands from the audit record on a local machine
- **THEN** the commands SHALL generate JSON and text reports under the change directory without requiring manual app interaction

#### Scenario: Manual tracing is needed
- **WHEN** a bottleneck requires live resize, typing, terminal, or startup trace confirmation
- **THEN** the audit SHALL list the exact `MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, `MICROIDE_PERF_TRACE_MIN_MS`, `MICROIDE_TRACE_REDRAW`, and `MICROIDE_TRACE_PROJECT_EVENTS` environment variables to use
