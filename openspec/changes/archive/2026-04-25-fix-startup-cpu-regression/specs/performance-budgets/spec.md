## MODIFIED Requirements

### Requirement: Startup Budget

MicroIDE SHALL complete cold startup (binary launch to interactive shell with the last-session project restored) within a documented budget on a reference Linux host, measured through `docs/startup-tracing.md`. Changes that affect startup paths or schedule startup-triggered background work SHALL record startup trace output and startup-to-idle evidence in the change record.

#### Scenario: Startup regression
- **WHEN** a change modifies `src/app/*` bootstrap, workspace-session restore, plugin runtime startup, syntax snapshot loading, project file-watching startup, or startup-triggered refresh scheduling
- **THEN** the change record SHALL include `MICROIDE_STARTUP_TRACE` output before and after the change plus corresponding evidence that the application settles to near-zero idle CPU after startup completes

#### Scenario: Last-session restore correctness
- **WHEN** the previous session held open editor tabs, compare tabs, merge tabs, and terminals
- **THEN** cold startup SHALL restore each of those surfaces to its recorded state as part of the budgeted startup path, not as a deferred post-startup task

### Requirement: Idle CPU Budget

When MicroIDE is loaded with a project but has no active user input, no running terminal output, and no in-flight background work, the application SHALL consume near-zero CPU and SHALL NOT produce unnecessary SDL wake events, including immediately after startup-triggered watcher or refresh activity has settled.

#### Scenario: Idle application
- **WHEN** MicroIDE has a project open, the user has not interacted for 30 seconds, and no background task is active
- **THEN** `top` or equivalent SHALL report effectively no CPU time attributed to the process, and the event loop SHALL be parked on an SDL wait rather than polling or repeated zero-delay timeout wakes

#### Scenario: Startup-triggered watcher activity
- **WHEN** project file watching, plugin asset watching, or other startup-triggered background refresh work is armed
- **THEN** each service SHALL either block on a native wake source or wait for a real poll deadline, and SHALL NOT keep the application in a zero-delay wake loop when no user-visible work is pending

### Requirement: Measured-Before-Merged Policy

Any change that modifies a hot render, text-layout, retained-scene, compare, merge, terminal, AI-hot path, file-watching path, or event-loop idle path SHALL include measured evidence (`MICROIDE_STARTUP_TRACE`, `MICROIDE_PERF_TRACE`, `microide_diff_bench`, or `microide_search_bench` output) in the change record before merge. Review SHALL NOT accept code-inspection-only claims of performance impact.

#### Scenario: Reviewer asked to accept an unmeasured perf claim
- **WHEN** a change claims a performance win or denies a performance risk without accompanying trace or bench output on a hot path
- **THEN** the reviewer SHALL request measured evidence before approving the change

#### Scenario: Idle regression risk
- **WHEN** a change touches project watchers, plugin watchers, scheduled wake routing, or background task polling
- **THEN** the change record SHALL include before-and-after startup trace output plus runtime or idle CPU evidence from a representative project-open run
