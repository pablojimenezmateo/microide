## 1. Commit State And UI

- [x] 1.1 Define commit workflow state, staged summary view model, check result model, and operation result model.
- [x] 1.2 Add commit workflow pane or surface using existing host-owned text input and button primitives.
- [x] 1.3 Wire Git sidebar `c`/commit action to open the commit workflow.

## 2. Draft Persistence

- [x] 2.1 Add repository/branch-scoped draft state.
- [x] 2.2 Persist drafts through `PersistenceService`.
- [x] 2.3 Clear drafts after successful commit for the matching context.

## 3. Checks And Execution

- [x] 3.1 Implement pre-commit checks for empty subject, long subject, unresolved conflicts, conflict markers, unstaged leftovers, branch behind, and untracked files.
- [x] 3.2 Execute commit, amend, and no-hook operations asynchronously.
- [x] 3.3 Capture hook output and display it in a native output panel.
- [x] 3.4 Add explicit confirmation for amend and no-hook actions.

## 4. Verification

- [x] 4.1 Add tests for staged summary, no staged files, draft restore/clear, blocking checks, warning acknowledgements, hook failure, and successful commit refresh.
- [x] 4.2 Add integration fixtures for commit with hooks, amend, no-hook, conflict markers, and staged/unstaged same-file changes.
- [x] 4.3 Run focused Git, persistence, workspace UI, and subprocess/background executor tests.
