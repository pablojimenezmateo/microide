## Why

Commit creation should be the final review step, not a plain text box bolted onto a sidebar. Users need staged summary, warnings, hook output, amend safety, and draft persistence to confidently move from a messy worktree to a clean commit.

## What Changes

- Add a commit workflow pane/surface with staged summary, commit message subject/body, staged diff access, branch/ahead/behind state, and explicit action buttons.
- Persist commit message drafts per repository through existing persistence seams.
- Add pre-commit checks for empty/long subject, unresolved conflicts, conflict markers, unstaged leftovers in staged files, branch behind warnings, and untracked files not included.
- Execute commit/amend/no-verify operations asynchronously, display hook output in a native panel, and publish structured failure results.
- Require explicit confirmation for amend and commit-without-hooks.

## Capabilities

### New Capabilities
- `commit-workflow`: Staged summary, commit draft, pre-commit checks, hook output, amend/no-hook actions, and structured commit failures.

### Modified Capabilities

## Impact

- Affects Git sidebar commit action, bottom/side commit pane UI, persistence of drafts, Git commit subprocess execution, hook output panel, repository refresh, and commit workflow tests.
- Depends on canonical Git repository snapshots for staged summaries and conflict state.
