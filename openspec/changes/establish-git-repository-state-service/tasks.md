## 1. Model And Parser

- [x] 1.1 Define `GitRepositoryState`, entry kinds, conflict kinds, operation state, refresh error categories, and generation metadata.
- [x] 1.2 Implement porcelain v2 `-z` status parsing with branch/upstream, rename, conflict, submodule, untracked, deleted, and typechanged fixtures.
- [x] 1.3 Add parser tests for paths with spaces, quotes, backslashes, UTF-8, and NUL-delimited rename pairs.

## 2. Service Pipeline

- [x] 2.1 Add `GitRepositoryService` behind a narrow workspace/project service interface.
- [x] 2.2 Dispatch refresh work through `ProjectBackgroundExecutor` or an equivalent non-main-thread executor.
- [x] 2.3 Add generation IDs, stale-state tracking, refresh coalescing, cancellation on project switch, and structured failure results.
- [x] 2.4 Publish SDL user events or existing wake events when a newer snapshot is ready.

## 3. Consumer Migration

- [x] 3.1 Route Git sidebar status rendering through the shared snapshot.
- [x] 3.2 Route status-bar branch/cleanliness state through the shared snapshot.
- [x] 3.3 Route compare, merge, and outgoing-file openers through snapshot-derived repository state where applicable.
- [x] 3.4 Remove superseded ad hoc Git status maps and direct workspace Git subprocess calls.

## 4. Verification

- [x] 4.1 Add regression tests for stale refresh completion, coalesced file-watch bursts, and repository-lock failure handling.
- [x] 4.2 Extend architecture lint to reject new synchronous Git subprocess calls in workspace hot paths if existing rules do not cover them.
- [x] 4.3 Run focused Git service, workspace sidebar, compare/merge opener, and architectural-lint tests.
- [x] 4.4 Run the Git sidebar perf scenario and record advisory output for refresh/activation paths touched by the change. (Deferred: `git_sidebar_refresh_large_repo` lands in `expand-git-diff-merge-perf-gates`; service refresh path is covered by workspace regression tests.)
