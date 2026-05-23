## Why

Branch review needs memory: users should know which files and hunks they already inspected, what changed since review, and what needs another pass. Local review state makes MicroIDE a workstation rather than just a diff viewer, while avoiding hosted-provider/auth scope.

## What Changes

- Add local review state for branch review: reviewed files, reviewed hunk hashes, optional notes, and changed-since-reviewed markers.
- Persist review state through the structured persistence service as project-scoped data.
- Associate review entries with repository root, base ref, head ref/commit, file path, diff generation, and hunk identity.
- Show reviewed/unreviewed/changed-since-reviewed markers in branch review file lists and compare tabs.
- Keep the feature local-only; no GitHub/GitLab comments, network sync, or auth.

## Capabilities

### New Capabilities
- `branch-review-state`: Local reviewed-file/hunk/note state for branch review workflows.

### Modified Capabilities
- `persisted-state-format`: Adds typed project-scoped records for local review state.

## Impact

- Affects branch review UI, compare file lists, persistence records, project state load/save, review-state tests, and stale marker behavior after diff changes.
- Depends on explicit branch review mode from `expand-diff-review-workflows`.
