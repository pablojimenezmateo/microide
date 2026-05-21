## Why

Git workstation correctness depends on trust under external mutation: files change outside the app, branches switch in terminals, build tools rewrite outputs, and merge states can change while tabs are open. The app needs one explicit external-change contract across repository state, editor buffers, diff tabs, merge tabs, file tree, search, diagnostics, and blame.

## What Changes

- Wire file-watch and repository-change signals into Git status refresh, open buffers, diff tabs, merge tabs, file tree, search index, diagnostics, blame, and commit draft invalidation where applicable.
- Define clean-buffer reload, dirty-buffer conflict prompt, stale diff refresh, stale merge result validation, and external branch-change behavior.
- Preserve scroll/caret anchors where safe after refresh.
- Prevent external changes from silently overwriting dirty user edits or marking merge results resolved.

## Capabilities

### New Capabilities
- `external-repo-change-handling`: File-watch and external repository mutation behavior for the Git/diff/merge/editor workflow.

### Modified Capabilities

## Impact

- Affects native file-watch handling, project tree refresh, Git snapshot invalidation, editor reload prompts, compare/merge tab refresh, diagnostics/blame stale-path cleanup, search index refresh, and regression tests for external mutation.
- Performance-sensitive because watcher bursts must not create subprocess storms or idle CPU regressions.
