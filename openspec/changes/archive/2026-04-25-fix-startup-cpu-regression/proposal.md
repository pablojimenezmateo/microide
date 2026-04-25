## Why

Recent commits introduced a startup regression where MicroIDE reaches 100% CPU immediately after opening and stays hot while apparently idle. This violates the idle CPU budget and makes the product feel broken; it also indicates that startup-triggered background work is no longer being measured or bounded tightly enough.

## What Changes

- Investigate the startup-to-idle path and identify which recent change causes the busy-loop or wake-event storm, with special attention to project file watching, event-loop wakeups, and startup-triggered background refresh work.
- Restore near-zero idle CPU after startup by making the offending watcher, scheduler, or render path block correctly when no real work is pending.
- Capture before-and-after startup and runtime measurements using the existing tracing and profiling workflow so the regression is explained with data rather than suspicion.
- Record the project's current startup and idle performance posture as part of the change so future regressions have a concrete reference point.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `performance-budgets`: tighten the startup and idle CPU requirements so startup-triggered watcher/background activity must remain bounded, measurable, and free of unnecessary wake-event storms.

## Impact

- Affected code will likely include startup initialization, the main event loop, project refresh/file watching, and any background scheduling paths activated during project open.
- Affected documentation includes `docs/startup-tracing.md` and `docs/runtime-profiling.md` through the measured evidence recorded for this regression.
- Affected product contract includes `openspec/specs/performance-budgets/spec.md`.
