## Why

A Git workstation is incomplete if users must leave the app to stage or discard parts of a file. File-level staging exists, but hunk-level and selected-line operations need a safe patch application service with previews and stale-state handling.

## What Changes

- Add patch staging operations for stage file, stage hunk, stage selected lines, unstage file, unstage hunk, unstage selected lines, discard hunk, and discard selected lines.
- Add a `PatchApplyService` boundary that builds patches from diff selections, previews destructive operations, applies patches to the index or worktree, and refreshes repository state.
- Detect stale diffs and patch-application failures without silently retrying against changed files.
- Keep discard operations more conservative than staging, with explicit previews and confirmation.

## Capabilities

### New Capabilities
- `patch-staging-operations`: Hunk/line staging, unstaging, discard semantics, patch preview, and failure handling.

### Modified Capabilities
- `workspace-architecture`: Adds patch application as a narrow service boundary instead of sidebar or compare-surface ad hoc Git logic.

## Impact

- Affects compare selection mapping, Git action dispatch, background Git apply/index operations, confirmation prompts, repository refresh, tests for patch generation/application, and perf coverage for large patches.
- Depends on reliable diff line mapping from `expand-diff-review-workflows` for selected-line staging quality.
