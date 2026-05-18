## Context

Current CI signal is degraded: required workflows (`host-platform`, `perf-harness`) are frequently failing, and scheduled runs (`perf-harness`, `fuzz`) add recurring failure noise that does not align with merge-candidate validation. The repository policy prioritizes correctness first and treats CI as the merge contract, so CI failures must be deterministic, actionable, and tied to concrete integration events (push/PR/manual dispatch) rather than periodic background traffic.

This change spans workflow triggers, workflow job configuration, and CI-facing specs. It also requires a repeatable triage path so existing failures are fixed as part of the change rather than deferred.

## Goals / Non-Goals

**Goals:**
- Restore reliable CI/CD checks for merge candidates by fixing current failing workflow jobs.
- Remove periodic/scheduled workflow triggers from repository workflows.
- Preserve required quality gates (host-platform checks, perf harness gate behavior, sanitizer/fuzz coverage) on push/PR/manual invocations.
- Define a durable CI reliability capability so trigger and triage policy remains explicit.

**Non-Goals:**
- Redesign the entire CI topology beyond what is needed for reliability and schedule-trigger removal.
- Introduce new external CI platforms or non-GitHub workflow systems.
- Relax required quality checks to make runs pass by reducing coverage.

## Decisions

### Decision: Use push/PR/workflow_dispatch triggers only
All active workflows will remove `schedule` triggers and run only from developer-initiated or merge-candidate events.

- **Rationale:** Eliminates recurring non-actionable failures and aligns CI cost/noise with code changes.
- **Alternatives considered:**
  - Keep schedules with reduced cadence: still creates background failure noise and does not satisfy explicit requirement.
  - Keep schedules but non-blocking: still consumes resources and obscures true merge-candidate health.

### Decision: Fix failures via root-cause triage per workflow
Each failing workflow will be inspected with `gh run view --log-failed`, and fixes will be applied to workflow definitions or invoked scripts/tests until required checks pass.

- **Rationale:** Avoids speculative pipeline edits and ensures durable fixes tied to observed failure modes.
- **Alternatives considered:**
  - Blanket retry/widen timeouts: can hide defects and increase CI latency without resolving root causes.
  - Disable failing jobs: violates quality-gate intent.

### Decision: Keep performance and bug-detection gates, but remove nightly assumptions
Spec-level requirements that mandate nightly/scheduled execution will be updated to require bounded execution on merge-candidate or manual trigger paths.

- **Rationale:** Preserves validation intent while respecting the no-periodic-workflow policy.
- **Alternatives considered:**
  - Remove long-running checks entirely: reduces confidence in regressions.
  - Keep nightly requirements in spec but not implementation: creates contract drift.

## Risks / Trade-offs

- **[Risk] Loss of passive background coverage from nightly runs** -> **Mitigation:** retain manual dispatch paths and ensure merge-candidate checks remain strong and deterministic.
- **[Risk] CI duration increases on push/PR after moving checks from schedule-only contexts** -> **Mitigation:** keep bounded runtime limits and use explicit smoke/full split where already supported by specs.
- **[Risk] Workflow fixes overfit current failures** -> **Mitigation:** validate with multiple reruns and codify trigger/gate behavior in specs and docs.

## Migration Plan

1. Collect latest failing runs for each active workflow and record root causes.
2. Patch workflow YAML and any dependent scripts/config used by failing jobs.
3. Remove all `schedule` trigger blocks from active workflows.
4. Re-run workflows on branch and verify required checks become green or deterministically actionable.
5. Update specs and CI documentation to reflect trigger policy and validation expectations.
6. If regression appears, rollback by reverting workflow/script commits and restoring prior known-good state for required checks (without reintroducing schedule triggers).

## Open Questions

- Should any former nightly-only check be preserved as a dedicated on-demand workflow (manual dispatch only), or should it be merged into existing push/PR pipelines with bounded runtime?
- Which checks are branch-protection-required today versus advisory, and should that set be updated after stabilization?

## Failure Matrix (2026-05-08)

| Workflow | Run | Event | Observed failure | Classification | Owner fix path |
| --- | --- | --- | --- | --- | --- |
| `host-platform` | `25548908209` | `push` | Job never started; check annotation reports account payment/spending-limit block | Environment/runner issue | Repository billing owner: restore GitHub Actions billing so hosted runners can be provisioned |
| `perf-harness` | `25548761157` | `push` | Job never started; check annotation reports account payment/spending-limit block | Environment/runner issue | Repository billing owner: restore GitHub Actions billing so hosted runners can be provisioned |
| `fuzz` | `25536109667` | `schedule` | Job never started; check annotation reports account payment/spending-limit block | Environment/runner issue | Repository billing owner + workflow owner: restore billing and remove schedule-only dependency to reduce recurring failure noise |

The current deterministic failures are external to repository code/config execution and block reliable in-repo verification until billing is restored. This change still removes periodic triggers and re-routes extended checks to manual/event-driven execution so CI policy and noise profile align with the new specs.

Verification reruns after this change (`25548908209`, `25548761157`, `25536109667`) still fail before job start with the same billing annotation, confirming a non-code blocker remains.
