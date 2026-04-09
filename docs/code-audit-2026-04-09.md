# Code Audit 2026-04-09

This document captures the same-day code audit after the larger terminal, search, source-control,
copy-with-context, tab-reordering, and editor blame iterations.

The goal was to identify correctness risks, dead code, rushed shortcuts, and cleanup debt before
they harden into project-wide patterns.

## Priority Findings

### 1. Stale in-flight blame work could repopulate cleared cache

Severity: high

Affected area:

- `src/project/GitBlameService.cpp`

Problem:

- `InvalidatePath()` and `Clear()` removed cached blame data, but they did not make the current
  worker request obsolete.
- If a request had already passed invalidation and was about to publish results, it could recreate
  a cache entry after the caller had explicitly cleared it.
- That could briefly show stale blame and, worse, suppress an immediate fresh request because the
  cache looked populated again.

Remediation:

- Version blame requests against path-level invalidations and global clears.
- Drop worker results whose version is no longer current before they mutate cache state.
- Keep regression coverage for invalidation during in-flight work.

Status:

- Being addressed immediately after this audit.

### 2. Bulk git discard did not fully reconcile open editor state

Severity: medium

Affected area:

- `src/workspace/WorkspaceShellSidebar.cpp`
- `src/workspace/WorkspaceShellPrompts.cpp`
- `src/workspace/WorkspaceShell.cpp`

Problem:

- `DiscardAllGitSidebarEntries()` only tracked paths that existed before the discard.
- That misses files restored from `HEAD` and does not close tabs for files removed by `git clean`.
- The UI can therefore report a successful discard while some open tabs still reflect the old
  working tree.

Remediation:

- Reconcile open tabs against the full set of affected git paths, not only the paths that existed
  before discard.
- Reuse the existing close/reload-by-path helpers instead of keeping separate partial behavior.

### 3. Blame cache invalidation was broader than intended

Severity: medium

Affected area:

- `src/workspace/WorkspaceShellSidebar.cpp`
- `src/workspace/WorkspaceShellProjects.cpp`
- `src/workspace/WorkspaceShellPrompts.cpp`

Problem:

- `RefreshGitSidebar()` currently clears all editor blame state, not just the paths affected by the
  operation that triggered the refresh.
- This keeps the feature correct, but it throws away most of the benefit of the viewport-scoped
  blame cache.

Remediation:

- Narrow invalidation to the paths affected by stage, unstage, discard, rename, delete, and save.
- Reserve full clears for project switches, root resets, or global git state loss.

### 4. Terminal transcript copy still relies on a width-based wrap heuristic

Severity: medium

Affected area:

- `src/workspace/WorkspaceShellTerminal.cpp`

Problem:

- The current “walk backward while the previous row fills the terminal width” logic is only a
  heuristic for wrapped commands.
- A full-width output line can be misclassified as part of the invocation.

Remediation:

- Track terminal prompt/invocation boundaries more explicitly, or keep the feature scoped as a best
  effort and document the limitation.

### 5. Git shell helpers are duplicated across services

Severity: low

Affected area:

- `src/project/GitStatusService.cpp`
- `src/project/GitCompareService.cpp`
- `src/project/GitBlameService.cpp`

Problem:

- Shell escaping, command execution, and git-root detection now exist in multiple near-identical
  implementations.
- This is not a correctness bug today, but it is maintenance debt.

Remediation:

- Consolidate shared git process helpers into one internal utility so behavior changes land once.

## Audit Notes

- No obvious dead code was found in the reviewed same-day change set.
- The main cleanup pressure is around invalidation correctness, state reconciliation, and reducing
  shortcut heuristics where they now touch durable user data.
- Verification at audit time:
  - `cmake --build build`
  - `ctest --test-dir build --output-on-failure`
  - `git diff --check`
