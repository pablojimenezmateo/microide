## 1. Audit Planning And Instrumentation

- [x] 1.1 Define the hotspot-audit matrix covering startup, typing, scrolling, search, terminal output, plugin-triggered updates, and idle behavior.
- [x] 1.2 Confirm deterministic harness inputs for all audited workflows (fixtures, seeds, frame pumping, plugin enablement rules).
- [x] 1.3 Add any missing lightweight measurement seams needed to isolate opaque workspace hot paths.

## 2. Perf Harness Coverage Expansion

- [x] 2.1 Run current `microide_perf_tests` scenarios and capture the baseline hotspot map (CPU, frame-time, wake-up, allocation, RSS trends).
- [x] 2.2 Add new perf scenarios and baselines for hotspot classes not currently covered by deterministic tests.
- [x] 2.3 Update harness documentation and scenario metadata so each new hotspot scenario is discoverable and reproducible.

## 3. Opportunity Ranking And Optimization Slices

- [x] 3.1 Build a ranked hotspot opportunity ledger with expected impact, complexity, and subsystem ownership.
- [x] 3.2 Implement top-ranked optimization slice(s) with narrow, service-safe changes that preserve architecture invariants.
- [x] 3.3 Add or tighten targeted regression tests for each implemented optimization slice.

## 4. Validation, Budgets, And Change Hygiene

- [x] 4.1 Re-run affected perf scenarios and confirm movement against per-metric tolerances; update baselines only with explicit rationale.
- [x] 4.2 Verify adjacent workflow scenarios remain within budgets when shared infrastructure is changed.
- [x] 4.3 Update performance docs/spec references and include `perf-baseline:` justification lines for any committed baseline movement.
