## ADDED Requirements

### Requirement: Hotspot Opportunity Ledger Is Budget-Aware
A hotspot-audit change SHALL map each high-priority finding to one or more existing or newly added budgeted scenarios, and SHALL define acceptance criteria in terms of measurable budget movement.

#### Scenario: Ranked hotspot has no budget mapping
- **WHEN** an optimization candidate is marked high priority
- **THEN** the change SHALL document which scenario metrics define success and SHALL add or update a budgeted scenario when no suitable mapping exists

### Requirement: Throughput-Oriented Performance Pass Is Measured Before Merge
Changes produced by a hotspot pass SHALL include measured-before-merge evidence for both targeted and adjacent hot paths to prevent regressions hidden by local improvements.

#### Scenario: Optimization improves one path but risks another
- **WHEN** a hotspot optimization modifies shared infrastructure used by multiple interactive workflows
- **THEN** the author SHALL run and cite harness scenarios for the targeted workflow and at least one adjacent workflow that shares the modified path
