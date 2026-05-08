## Why

Recent GitHub Actions runs for `host-platform`, `perf-harness`, and `fuzz` are repeatedly failing, which keeps the default branch in a noisy and unreliable state. We need to restore deterministic merge-candidate validation and remove periodic/scheduled workflows that are not required.

## What Changes

- Audit current failing GitHub Actions runs and classify root causes by workflow and job.
- Fix CI pipeline failures so required push/PR checks are reliable and actionable.
- Remove periodic (`schedule`) workflow triggers and keep CI execution on push/PR/manual paths only.
- Update CI documentation and guardrails so removed periodic triggers do not regress back.

## Capabilities

### New Capabilities
- `ci-workflow-reliability`: Define deterministic CI workflow behavior, failure triage expectations, and trigger policy for required validations.

### Modified Capabilities
- `performance-harness`: Update harness CI requirements to remove periodic schedule expectations while preserving merge-candidate regression gating.
- `bug-detection-tooling`: Remove nightly/scheduled fuzz and soak requirements and redefine when bounded sanitizer/fuzz/soak validations run without periodic triggers.

## Impact

- Affected systems: GitHub Actions workflow files under `.github/workflows/`, CI scripts invoked by workflow jobs, and CI-related documentation.
- Affected contracts: `openspec/specs/performance-harness/spec.md` and `openspec/specs/bug-detection-tooling/spec.md`, plus a new capability spec for CI reliability policy.
- Expected outcome: fewer false-negative checks, predictable merge gating, and no recurring schedule-triggered workflow load.
