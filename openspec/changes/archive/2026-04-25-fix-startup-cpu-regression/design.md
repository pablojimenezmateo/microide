## Context

MicroIDE currently computes the next main-loop wake from `WorkspaceShell::NextAnimationDelayMs()` and runs scheduled work through `WorkspaceShell::HandleScheduledWake()`. Project file watching is armed during `WorkspaceShell::SetProjectRoot()`, while the underlying `platform::FileTreeWatcher` can use a native Linux backend or fallback snapshot polling. Plugin asset watching already wraps the same watcher with an SDL wake-event coalescing layer, but the project watcher is still consumed through the generic scheduled-wake path.

The reported regression is severe: the app reaches 100% CPU immediately after opening and remains hot while the user is idle. The likely failure mode is a startup-to-idle wake loop caused by watcher notifications, repeated zero-delay scheduled wakes, or a refresh path that continuously re-arms itself. The existing performance contract requires startup and idle measurements, but it does not explicitly call out startup-triggered watcher and event-loop changes strongly enough.

## Goals / Non-Goals

**Goals:**
- Identify the exact source of the startup-to-idle CPU regression using trace data instead of a speculative local patch.
- Restore near-zero idle CPU after project open while preserving correct project refresh behavior.
- Make project file watching wake the UI in a coalesced, bounded way instead of relying on repeated zero-delay scheduled wake checks when native notifications are available.
- Record before-and-after startup and runtime evidence so the change also serves as a current project performance evaluation.

**Non-Goals:**
- Redesign every background service or wake source in the application.
- Change product behavior for unrelated redraw, terminal, AI, or plugin-runtime paths unless the regression investigation proves they are involved.
- Introduce new performance benchmarks beyond the existing startup trace, runtime trace, and project-representative validation workflow.

## Decisions

### 1. Measure the startup-to-idle path before fixing it

The change will start by capturing trace evidence around startup, scheduled wakes, and idle behavior on the current tip and on a known-good baseline. The investigation should focus on `Application::Run`, `WorkspaceShell::NextAnimationDelayMs()`, `WorkspaceShell::HandleScheduledWake()`, and `platform::FileTreeWatcher`.

Why this approach:
- The user only has a hypothesis that the file watcher is involved.
- The regression is severe enough that a single hot loop or repeated wake source should be detectable quickly with existing tracing and a small amount of extra targeted instrumentation if needed.

Alternatives considered:
- Speculatively increasing poll intervals or disabling the watcher. Rejected because it can hide the regression instead of fixing the incorrect wake behavior.

### 2. Give the project watcher an explicit coalesced SDL wake path

The project watcher should follow the same host-owned pattern already used by `WorkspacePluginAssetMonitor`: reserve at most one pending SDL wake event, consume that event on the UI thread, and only then poll and refresh project state. `NextAnimationDelayMs()` should only consider the project watcher when a real fallback poll deadline exists; native watcher notifications should wake through the event queue instead of through repeated zero-delay timeouts.

Why this approach:
- It keeps the event loop parked on `SDL_WaitEvent` unless there is real work or a real fallback deadline.
- It makes repeated file-system notifications cheap by coalescing them.
- It creates a clearer seam for regression tests around watcher wake behavior.

Alternatives considered:
- Keeping the current scheduled-wake polling pattern and just raising the poll interval. Rejected because it increases latency and still leaves the event loop behavior coupled to timeout churn.

### 3. Refresh project state only after a confirmed change and keep settle work bounded

Watcher hints should remain hints. After a wake, the UI thread should perform one `Poll()`/snapshot reconciliation pass, refresh the directory tree and clean buffers only when the project snapshot actually changed, and then return to the blocked event wait. Startup-triggered one-time settle work must not continuously requeue itself once the project state is stable.

Why this approach:
- Native file notifications can arrive in bursts.
- The app should not convert every watcher hint into a full project refresh and redraw.
- The bounded-settle rule preserves correctness while protecting idle CPU.

Alternatives considered:
- Full project refresh on every watcher notification. Rejected because it needlessly amplifies bursty file-system traffic into redraw and CPU churn.

### 4. Treat the fix as both a bug fix and a project performance evaluation

The change record should capture before-and-after `MICROIDE_STARTUP_TRACE` output and corresponding runtime or idle CPU evidence on a representative project open. That turns this regression fix into a fresh performance checkpoint for the current project state rather than a narrow anecdotal fix.

Why this approach:
- The user explicitly asked to evaluate project performance in addition to fixing the regression.
- The existing docs already define the preferred measurement surfaces, so the added cost is small and the value for future regressions is high.

Alternatives considered:
- Declaring the regression fixed based on subjective observation alone. Rejected because the project’s own performance budget forbids inspection-only claims on hot paths.

## Risks / Trade-offs

- [Risk] The root cause may not be the project watcher and the design could overfit that hypothesis. → Mitigation: begin with trace-driven investigation and only commit to the watcher refactor once the hot wake source is confirmed.
- [Risk] Coalescing watcher wakes could delay or collapse intermediate file-system events. → Mitigation: keep `Poll()` as the source of truth and coalesce only the UI wake notification, not the underlying snapshot reconciliation.
- [Risk] Linux-native and polling fallback behavior could diverge. → Mitigation: preserve one shared `FileTreeWatcher` reconciliation path and add targeted tests for both pending-wake and poll-deadline behavior.
- [Risk] Added tracing could itself perturb startup slightly. → Mitigation: keep new instrumentation env-gated and use the existing tracing surfaces where possible.

## Migration Plan

1. Capture current-tip startup and idle evidence on a representative project open.
2. Add or extend targeted tracing if the existing startup/runtime output is not enough to identify the hot wake path.
3. Refactor the project watcher integration so native file changes wake the UI through a coalesced SDL event instead of a timer-driven zero-delay path.
4. Re-run startup and idle measurements, confirm the application blocks correctly when stable, and keep any regression coverage introduced by the refactor.

## Open Questions

- Which exact recent commit introduced the hot wake loop?
- Does the final implementation warrant a small shared watcher-wake helper for both project and plugin monitoring, or is a project-specific wrapper cleaner?
- Which representative project should be used as the durable before/after measurement fixture for this change record?
