## 1. Reproduce And Measure The Regression

- [x] 1.1 Capture current-tip startup trace and idle CPU/runtime evidence on a representative project-open run.
- [x] 1.2 Identify the hot wake source across `Application::Run`, `WorkspaceShell` scheduled wakes, and `platform::FileTreeWatcher`, adding env-gated tracing only if the existing profiling surfaces are insufficient.

## 2. Fix The Startup-To-Idle Wake Path

- [x] 2.1 Refactor project file-watcher integration so native watcher notifications wake the UI through a coalesced SDL event path instead of repeated zero-delay scheduled wake checks.
- [x] 2.2 Ensure project refresh, clean-buffer reload, and redraw work only run after a confirmed watcher change and do not continuously re-arm once startup settle work is complete.
- [x] 2.3 Add or tighten regression coverage for project watcher wake coalescing, poll-deadline behavior, and idle-state stability after project open.

## 3. Validate And Record Project Performance

- [x] 3.1 Re-run startup and runtime measurements after the fix and compare them against the baseline collected in task 1.
- [x] 3.2 Update the change record and any affected performance docs with the before/after evidence and the final performance evaluation summary.
