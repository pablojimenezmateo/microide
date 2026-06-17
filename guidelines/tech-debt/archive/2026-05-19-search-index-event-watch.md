# Search and index integration — event-driven file watch

- Date: 2026-05-19
- Area: project, platform
- Source: §5; `deferred-work-and-throughput-pass` plus the `perf/project-search-lower-snapshot`
  follow-up; 2026-05-20 scoped async tree git-status refresh

## Summary

Project search/index integration moved off per-run disk rescans onto an event-driven
`FileIndexWatcher` layer with a background executor, shared path snapshots, and incremental result
deltas.

## Resolution

What was closed:
- `FileIndexWatcher` platform abstraction ships Linux `inotify`, macOS `FSEvents`, Windows
  `ReadDirectoryChangesW`, and a poll-fallback backend.
- `PatternCache` with LRU eviction eliminates repeated PCRE2 compile/JIT on repeated searches.
- `ProjectBackgroundExecutor` isolates per-project git dispatch from the main thread.
- `BackgroundTaskCounter` tracks in-flight background work for adaptive idle rendering.
- PCRE2 JIT is compiled into the search engine; interpreted fallback emits a one-time log.
- `ProjectSearchService` wires `BackgroundTaskCounter` so the event loop stays awake during search.
- Workspace project-state wiring starts/stops the file-index watcher with project lifecycle, and
  file-index updates invalidate/refresh dependent file-finder and search state.
- File finder reads from `FileIndex` snapshots instead of rescanning the tree on each refresh.
- Project search starts from `FileIndex::SnapshotPathsWithVersion(...)`, reuses a single lowercase
  line buffer in case-insensitive literal mode, and publishes incremental result deltas instead of
  snapshotting the entire cumulative result set back to the shell on every consume.
- Blame and log dispatch are off the UI thread.

Closed on 2026-05-20 (scoped async tree git-status): `DirectoryTree::RefreshGitStatuses()` no longer
runs during project set-root, and `WorkspaceSidebarCoordinator::ShowGit()` no longer calls it
synchronously. The tree status map is built from the existing async Git sidebar working-tree snapshot
and applied through `DirectoryTree::ApplyGitStatuses()`. After first paint, a scoped async refresh
materializes tree badges without blocking startup. Automatic status-only refreshes still skip tree
badge materialization until that first-paint hook runs.

### Investigation note (2026-05-19 perf-compare diagnostic) — kept for archaeology

An unconditional `RefreshGitSidebar` on project set-root cost +500k–1.4M allocations across most
project-open scenarios and +27–30% wall on cold-startup fixtures. Root cause: each
`RequestGitSidebarRefresh` posts a worker task spawning 4 git subprocesses
(`CollectGitWorkingTreeEntries`, `ResolveGitOutgoingBase`, `ResolveGitBranchLabel`,
`CollectGitBranchOutgoingFiles`); subprocess setup costs ~100k+ short-lived allocations per spawn.
The allocations free promptly (RSS does not regress), but the churn is pure overhead when the user
has not opened the Git sidebar. **Lesson:** "async" does not mean "free" — a background task that
allocates ~480k strings on every project open still bills the allocator counter and the cold-startup
wall budget. Future async migrations should be gated on user-visible demand or measured to be
allocation-cheap before being made unconditional.
