## 1. CI Failure Triage Baseline

- [x] 1.1 Capture the latest failing `host-platform`, `perf-harness`, and `fuzz` runs with `gh run list` and `gh run view --log-failed`.
- [x] 1.2 Classify each failure into workflow-config error, environment/runner issue, or test/script defect and map each to an owner fix path.
- [x] 1.3 Document a short failure matrix in the change notes so remediation scope is explicit before patching.

## 2. Workflow Trigger Cleanup

- [x] 2.1 Remove all `schedule` trigger blocks from active workflows under `.github/workflows/`.
- [x] 2.2 Ensure each retained workflow uses only `push`, `pull_request`, and/or `workflow_dispatch` triggers.
- [x] 2.3 Add or update workflow comments/docs that encode the non-periodic trigger policy.

## 3. Required Workflow Reliability Fixes

- [ ] 3.1 Patch `host-platform` workflow/job configuration and dependent scripts to eliminate current deterministic failures.
- [ ] 3.2 Patch `perf-harness` workflow/job configuration and dependent scripts to eliminate current deterministic failures while preserving gate semantics.
- [ ] 3.3 Patch `fuzz` workflow/job configuration and dependent scripts so bounded merge-candidate or manual runs execute reliably without schedule reliance.

## 4. Spec And Documentation Alignment

- [x] 4.1 Implement the new `ci-workflow-reliability` spec requirements in CI workflow definitions and any related docs.
- [x] 4.2 Apply the `performance-harness` spec delta to enforce non-periodic trigger policy and unchanged merge gating behavior.
- [x] 4.3 Apply the `bug-detection-tooling` spec delta to remove nightly assumptions and route extended checks to manual/event-driven execution.

## 5. Verification And Sign-off

- [x] 5.1 Re-run all affected workflows after fixes and confirm deterministic green runs or clearly documented non-code blockers.
- [x] 5.2 Verify no active workflow contains a periodic schedule trigger.
- [x] 5.3 Update `docs/active-work.md` and/or CI-related docs with the final trigger policy and stabilization outcomes.
