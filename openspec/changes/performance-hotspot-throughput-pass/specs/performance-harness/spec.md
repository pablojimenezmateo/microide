## ADDED Requirements

### Requirement: Hotspot Coverage Expansion Is Mandatory
The performance harness SHALL maintain explicit scenario coverage for any hotspot class identified by an approved hotspot-audit pass. New hotspot classes SHALL NOT remain untracked after the pass completes.

#### Scenario: Hotspot class lacks scenario coverage
- **WHEN** the hotspot-audit ledger includes a class not represented by current scenarios
- **THEN** the change SHALL add at least one deterministic `tests/perf/scenarios/<name>.cpp` entry and a matching `tests/perf/baselines/<name>.json` baseline in the same change

### Requirement: Hotspot Scenarios Capture Multi-Metric Evidence
Scenarios introduced for hotspot coverage SHALL capture frame-time percentiles, wall time, CPU time, redraw counts, wake-up counts, allocation counts, and RSS movement so regression triage can distinguish rendering, scheduling, and memory causes.

#### Scenario: New hotspot scenario reports full metric set
- **WHEN** a hotspot-protection scenario is executed
- **THEN** its baseline and output SHALL include all standard harness metrics required by the structured metric capture contract and SHALL be reviewable in pull request diffs
