## Why

Git status, compare, merge, outgoing, staging, and commit surfaces need one authoritative repository snapshot so the UI cannot show contradictory state or act on stale Git output. This establishes the correctness foundation for the Git workstation wedge before adding richer diff, merge, staging, and commit workflows.

## What Changes

- Add a `GitRepositoryState` snapshot model that represents branch identity, upstream relationship, ahead/behind counts, worktree status, staged status, untracked files, conflicts, submodules, ongoing operations, refresh errors, and generation metadata.
- Add a service-owned async refresh pipeline that uses stable machine-oriented Git output, NUL-delimited path parsing, cancellation/coalescing, and generation checks.
- Route Git sidebar, compare/merge openers, status-bar source-control state, and future commit/staging flows through the shared snapshot instead of direct ad hoc Git queries.
- Surface refreshing, stale, and failed snapshot state to UI callers.
- Keep Git subprocess execution off render, input, and workspace hot paths.

## Capabilities

### New Capabilities
- `git-repository-state`: Authoritative Git repository snapshots, async refresh semantics, parsing rules, and stale-result handling.

### Modified Capabilities
- `workspace-architecture`: Adds `GitRepositoryService` as a host-owned workspace service boundary for repository state and operations.

## Impact

- Affects `src/project/*` Git subprocess wrappers, `src/workspace/*Git*`, status bar source-control state, compare/merge target resolution, tests for Git status parsing, and background executor/event delivery.
- Adds regression fixtures for porcelain v2 parsing, NUL-delimited paths, rename/delete/typechange/submodule/conflict states, refresh coalescing, and stale generation rejection.
