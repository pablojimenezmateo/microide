## Why

Reopening a folder on a host means retyping `[user@]host[:port]:/path` in
Remote › Open Folder on Host… every time. Tab completes the host from
`~/.ssh/config`, but never the path, and the path is the long part. The editor
already remembers every remote project it opened (the persisted MRU behind the
welcome page's Recent list) — the host prompt just does not show them, so the one
place a user goes to open a host folder is the one place that has forgotten them.

## What Changes

- The Open Folder on Host… prompt shows a **Recent** section under its input:
  the latest remote folders opened, newest first, at most **5**, each as the
  `[user@]host[:port]:/path` it is reopened by.
- Clicking a row opens that folder (the same path Enter takes with that text).
  Up/Down cycles the rows, writing the row into the field, so Enter opens it and
  the user may still edit it first. The row equal to the field is highlighted;
  the row under the pointer is lifted.
- With no remote recents the prompt is exactly as before (same size, no heading).
- `RecentsService::RecentRemoteFolders(limit)` derives the list from the MRU; the
  prompt state carries it as a generic `recent_inputs` list, so nothing in the
  prompt is remote-specific beyond where it is filled.
- `ComputePromptSurfaceRect` takes the recent row count (no default, so every hit
  test, cursor, redraw, occluder and control-ui caller agrees with the paint), and
  `ComputePromptSurfaceRecentRowRects` lays the rows between the input and the
  buttons, dropping rows that do not fit a short window.
- The control channel's ui listing reports each row as a `prompt/recent/<n>` button.

## Impact

- `src/workspace/persistence/RecentsService.*`, `state/WorkspacePromptState.h`,
  `services/PromptSurfaceService.cpp`, `WorkspaceLayout.*`, the prompt's render,
  mouse, cursor, redraw, key and control-ui paths.
- Spec: `remote-connection-lifecycle` (host entry points).
- No persistence format change: the list is read from the existing MRU.
