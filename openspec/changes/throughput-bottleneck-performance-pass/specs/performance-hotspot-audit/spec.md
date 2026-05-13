## ADDED Requirements

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
