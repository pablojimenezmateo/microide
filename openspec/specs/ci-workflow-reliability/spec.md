# ci-workflow-reliability Specification

## Purpose
Define the CI workflow reliability contract for actionable, event-driven validation. Workflows must
avoid periodic schedules, fail with enough command context to reproduce issues, and require reruns
of affected workflow families when CI reliability patches land.

## Requirements
### Requirement: CI Trigger Policy Is Event-Driven
Repository CI workflows SHALL run from event-driven triggers (`push`, `pull_request`, or `workflow_dispatch`) and SHALL NOT use periodic `schedule` triggers.

#### Scenario: Workflow trigger audit
- **WHEN** CI workflows are reviewed for trigger configuration
- **THEN** each active workflow SHALL define only event-driven triggers and SHALL omit all cron-based schedule triggers

### Requirement: Failing CI Runs Are Actionable
Required CI workflows SHALL fail with logs that identify the failing job and failing command so remediation can be performed without ad hoc reproduction assumptions.

#### Scenario: Required workflow fails
- **WHEN** a required workflow run concludes with failure
- **THEN** the run logs SHALL identify the failing step and command with enough detail to reproduce or patch the issue in-repo

### Requirement: CI Stabilization Requires Verification Reruns
Any change that patches CI workflow reliability SHALL re-run the affected workflows and SHALL capture pass/fail status for the same workflow family before merge.

#### Scenario: Workflow reliability patch submitted
- **WHEN** a pull request includes CI workflow or CI-script fixes
- **THEN** the affected workflows SHALL be rerun on that pull request branch and SHALL show deterministic successful completion or a documented remaining blocker before merge
