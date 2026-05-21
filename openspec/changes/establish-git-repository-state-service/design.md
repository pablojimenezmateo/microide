## Context

The shipped baseline already has an async Git sidebar, stage/unstage/discard actions, compare/merge openers, outgoing-file views, and status-bar source-control affordances. Those surfaces are still product-coupled: each can become stale independently, and future hunk staging or commit checks need a coherent view of the index and worktree.

The implementation must preserve existing workspace architecture invariants: no broad `WorkspaceShell` ownership, no synchronous subprocess waits in workspace code, no render-time shell state reads, and no per-frame string work in render translation units.

## Dependencies

- Depends on: existing workspace architecture and background executor seams.
- Consumed by: `polish-git-sidebar-command-center`, `expand-diff-review-workflows`, `add-hunk-line-staging`, `harden-merge-resolution-workflow`, and `improve-commit-workflow`.
- Unblocks: canonical generation IDs and snapshot staleness checks across Git/diff/merge/commit flows.

## Goals / Non-Goals

**Goals:**
- Make `GitRepositoryService` the only owner of repository snapshot state.
- Parse repository state from machine-stable Git output with NUL-delimited paths.
- Deliver immutable snapshots with generation IDs so stale async completions are ignored.
- Expose explicit `refreshing`, `stale`, and `failed` states to view-model builders and command handlers.
- Add focused tests before moving richer staging, review, and commit flows onto the service.

**Non-Goals:**
- No hosted-provider integration, PR review, auth, or network Git operations.
- No hunk/line staging implementation in this change.
- No merge resolver UI expansion beyond consuming the shared snapshot where needed.
- No plugin SCM API expansion.

## Decisions

- `GitRepositoryService` owns immutable snapshots and publishes by generation. This favors correctness over low churn because every consumer can detect whether an operation was planned against an older repository state.
- Git parsing uses `status --porcelain=v2 -z --branch --renames` for the core status snapshot. This avoids locale-dependent output, path quoting, and user configuration surprises.
- Snapshot refreshes are coalesced per repository root. A new refresh request may mark the snapshot stale and schedule a newer generation, but it must not let older subprocess results overwrite newer state.
- Workspace callers receive snapshots through service APIs and view models, not by shell reach-through. Render code consumes prebuilt labels and flags from `RenderViewModelBuilder`.
- Failures are structured as service state, not status text assembled in UI handlers. UI surfaces can show concise status while retaining Git stderr for detail panels.

## Risks / Trade-offs

- Broad call-site migration can touch many workspace files -> keep the first implementation focused on status/outgoing/compare target consumers and leave hunk staging to a later change.
- Porcelain v2 parsing is more complex than the current status summary path -> add parser fixtures for every state class before routing UI actions through it.
- More frequent file-watch refreshes can create subprocess storms -> debounce, coalesce, and report active generation counts in tests/perf counters.
- Snapshot immutability may allocate more than mutating in place -> measure Git sidebar activation and refresh scenarios before merge, and prefer deleting duplicate status maps over caching extra copies.

## Migration Plan

1. Introduce service and parser types behind existing behavior.
2. Add tests that compare old and new status summaries for representative fixtures.
3. Route Git sidebar/status-bar reads to the service snapshot.
4. Route compare/merge/outgoing openers to the same snapshot where repository state is needed.
5. Delete superseded ad hoc Git status maps once all consumers are migrated.

## Open Questions

- Whether submodule detail should be fully parsed in the first slice or represented as a first-class but minimally populated status entry.
- Whether ahead/behind should be computed only on full manual refresh or also on automatic status-only refresh.
- Detached HEAD and unborn branch representation in the snapshot contract.
- Worktree repository handling and repository-root identity for multiple worktrees.
