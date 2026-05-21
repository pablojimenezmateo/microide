## Why

The Git sidebar should be the command center for the validated workflow: inspect changes, open diffs, resolve conflicts, stage or discard intentionally, and commit. The shipped sidebar already has many actions; this change turns them into a predictable keyboard-first grammar backed by explicit preview and safety states.

## What Changes

- Reorganize the Git sidebar into branch/upstream status plus grouped Conflicts, Staged, Changed, Untracked, and Outgoing sections.
- Add a consistent row action model for opening best view, opening diff, staging, unstaging, discarding, opening merge resolver, refreshing, committing staged work, and opening the working-tree file.
- Represent destructive actions as previewable flows with confirmation before data loss.
- Show snapshot stale/refresh/error states from the Git repository service.
- Preserve keyboard-first navigation and avoid direct Git subprocess work from sidebar input/render paths.

## Capabilities

### New Capabilities
- `git-sidebar-command-center`: Git sidebar grouping, row actions, destructive-action previews, and snapshot state presentation.

### Modified Capabilities

## Impact

- Affects Git sidebar view models, keyboard and mouse action routing, status labels, prompt/confirmation flows, discard/stage action dispatch, and sidebar tests.
- Depends on the shared repository snapshot contract from `establish-git-repository-state-service` for full correctness, but can be implemented incrementally against the current snapshot path.
