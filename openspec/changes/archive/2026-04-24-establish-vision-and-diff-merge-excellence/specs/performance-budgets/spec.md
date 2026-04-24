## ADDED Requirements

### Requirement: Startup Budget

MicroIDE SHALL complete cold startup (binary launch to interactive shell with the last-session project restored) within a documented budget on a reference Linux host, measured through `docs/startup-tracing.md`. Changes that affect startup paths SHALL record startup trace output in the change record.

#### Scenario: Startup regression
- **WHEN** a change modifies `src/app/*` bootstrap, workspace-session restore, plugin runtime startup, or syntax snapshot loading
- **THEN** the change record SHALL include `MICROIDE_PERF_TRACE` startup output before and after the change

#### Scenario: Last-session restore correctness
- **WHEN** the previous session held open editor tabs, compare tabs, merge tabs, and terminals
- **THEN** cold startup SHALL restore each of those surfaces to its recorded state as part of the budgeted startup path, not as a deferred post-startup task

### Requirement: Typing And Scrolling Frame Budget

Editor typing, editor scrolling, compare scrolling, merge scrolling, and terminal scrolling SHALL fit within a single-digit millisecond frame budget on the reference host. Changes that modify the text layout, syntax state, retained-scene redraw policy, or viewport scroll math SHALL include `MICROIDE_TRACE_REDRAW` before-and-after output.

#### Scenario: Typing into a large open file
- **WHEN** the editor has a file loaded that triggers the large-file code path and the user holds a printable key
- **THEN** each insertion SHALL render within the frame budget, SHALL NOT produce a full-surface repaint per keystroke, and SHALL NOT regress measurable typing latency compared to the prior release

#### Scenario: Scrolling a merge tab
- **WHEN** a three-way merge tab is scrolled with the mouse wheel or `PageDown`
- **THEN** each repaint SHALL fit within the frame budget and SHALL reuse the shared row-decoration cache rather than rebuilding decorations per frame

### Requirement: Idle CPU Budget

When MicroIDE is loaded with a project but has no active user input, no running terminal output, and no in-flight background work, the application SHALL consume near-zero CPU and SHALL NOT produce unnecessary SDL wake events.

#### Scenario: Idle application
- **WHEN** MicroIDE has a project open, the user has not interacted for 30 seconds, and no background task is active
- **THEN** `top` or equivalent SHALL report effectively no CPU time attributed to the process, and the event loop SHALL be parked on an SDL wait rather than polling

#### Scenario: Hover, caret, and animation paths
- **WHEN** a feature proposes adding animation, caret blink work, or hover-driven repaint
- **THEN** the proposal SHALL justify the wake-event cost under idle, and SHALL gate the wake on a real state change rather than a timer

### Requirement: Background Work Isolation

Git, search, blame, LSP, DAP, formatter, AI, and plugin background work SHALL NOT starve one another and SHALL NOT stall the render or input hot paths. Cancellation SHALL flow across project switch, tab close, and shutdown.

#### Scenario: Long-running AI request during git refresh
- **WHEN** the user triggers a git refresh while a chat response is streaming and a project search is running
- **THEN** all three SHALL progress concurrently, and MicroIDE SHALL continue to accept editor input at the frame budget

#### Scenario: Project switch cancels in-flight work
- **WHEN** the user switches projects while LSP requests, blame requests, search, and AI responses are in flight
- **THEN** each in-flight request SHALL be cancelled or ignored on completion, and the new project SHALL start with a clean set of background tasks

### Requirement: Measured-Before-Merged Policy

Any change that modifies a hot render, text-layout, retained-scene, compare, merge, terminal, or AI-hot path SHALL include measured evidence (`MICROIDE_PERF_TRACE`, `microide_diff_bench`, or `microide_search_bench` output) in the change record before merge. Review SHALL NOT accept code-inspection-only claims of performance impact.

#### Scenario: Reviewer asked to accept an unmeasured perf claim
- **WHEN** a change claims a performance win or denies a performance risk without accompanying trace or bench output on a hot path
- **THEN** the reviewer SHALL request measured evidence before approving the change
