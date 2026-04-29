## 1. Branch Resolution And Git Primitives

- [ ] 1.1 Add `GitRepository::ResolveBranch()` returning `{branch_or_empty, head_short_sha, is_detached}` via `git rev-parse --abbrev-ref HEAD` plus fallback to `git rev-parse --short HEAD`. Cover with focused fixtures including a detached-HEAD case.
- [ ] 1.2 Add a helper to detect whether a project root is a git worktree (file `.git` with `gitdir: …/worktrees/<name>` pointer) and to read back the originating repository root.
- [ ] 1.3 Add a helper to list local branches via `git for-each-ref refs/heads --format=%(refname:short)` for the create-worktree prompt's typeahead.

## 2. GitWorktreeService

- [ ] 2.1 Add `src/project/GitWorktreeService.{h,cpp}` with `WorktreeEntry`, `CreateWorktreeRequest`, `CreateWorktreeResult`, and the `CreateWorktree`, `ListWorktrees`, `RemoveWorktree`, `PruneWorktrees` functions.
- [ ] 2.2 Implement `ListWorktrees` by parsing `git worktree list --porcelain`, including `bare`, `detached`, `locked`, and `prunable` flags.
- [ ] 2.3 Implement `CreateWorktree` to assemble the correct argv (`-b <branch>` only when `create_branch == true`, base revision appended when non-empty), validate target path is empty/missing, and surface the verbatim git stderr in `error_message` on failure.
- [ ] 2.4 Implement `RemoveWorktree` and `PruneWorktrees`; ensure `RemoveWorktree` returns a structured "dirty, retry with force" signal distinguishable from other failures.
- [ ] 2.5 Add fixture coverage for: create with new branch, create against existing branch, create rejects when branch already checked out elsewhere, list parsing on a multi-worktree repo, remove-clean, remove-dirty-then-force, and prune-after-external-delete.

## 3. Project Tab Metadata And Persistence

- [ ] 3.1 Add `ProjectGitMetadata { is_worktree, origin_root, branch, branch_resolved_at }` to the project tab/state model.
- [ ] 3.2 Populate metadata at project-open time: detect worktree status, resolve branch, record `HEAD` mtime.
- [ ] 3.3 Extend `WorkspacePersistenceFormat` (or the structured-format equivalent if landed) with optional `worktree_origin` and `branch_cache` fields. Readers SHALL tolerate their absence; writers SHALL emit them when `is_worktree`.
- [ ] 3.4 Add round-trip fixtures: persist + reload a worktree project tab, persist + reload an older file lacking the new fields.

## 4. HEAD Watch And Async Refresh

- [ ] 4.1 Add a narrow `HEAD` file watch per project (correct path for plain repo vs. worktree) using the existing host-owned tree watcher; debounce callbacks by ~100 ms.
- [ ] 4.2 On `HEAD` change, run `ResolveBranch` on the background task executor; on result, post back to the main loop and update `ProjectGitMetadata` only if the resolved branch string changed.
- [ ] 4.3 Trigger a single tab-label invalidation when the branch changes; verify with a redraw fixture that no full-surface repaint occurs.
- [ ] 4.4 Cover commit-in-tab and `git checkout <other>` cases with fixtures using a temp-repo helper.

## 5. Tab Label Composition

- [ ] 5.1 Add `ProjectTabLabel`, `ProjectBreadcrumbLabel`, `ProjectTooltipLabel` to `WorkspaceProjectPresentation`. Use `·` separator; preserve the project name on truncation; tooltip is always full text.
- [ ] 5.2 Update tab and breadcrumb render call sites to consume the new label helpers.
- [ ] 5.3 Worktree tooltip second line includes origin path; detached tooltip includes `(detached HEAD)`.
- [ ] 5.4 Add render-level fixtures for: git-backed label, non-git label, detached label, very narrow tab width, worktree tooltip.

## 6. Create Worktree Prompt Flow

- [ ] 6.1 Add a `Worktree: Create…` command and a `Project → New Worktree…` menu entry, both wired through the existing command/menu registries; available only when the active project is git-backed.
- [ ] 6.2 Implement the four-step prompt (base / branch name / target path / confirm) using `PromptSurfaceService`. Step 1 typeahead uses the local-branch list helper from 1.3; step 2 validates against existing branches and existing worktrees; step 3 defaults to `<origin_parent>/<origin_basename>.worktrees/<sanitized-branch>`; step 4 shows the resolved git invocation.
- [ ] 6.3 Run the create-worktree call on the task executor; while pending, show a progress indicator in the prompt and keep the UI responsive. On success, open the new directory as a project tab and activate it.
- [ ] 6.4 Map known git failures to inline prompt errors: branch already checked out (offer "Open existing worktree"), invalid base, non-empty target path, bare repo. Keep the prompt open on error so the user can correct the input.

## 7. Open And Remove Worktree Commands

- [ ] 7.1 Add a `Worktree: Open…` command that lists worktrees of the active repository (excluding any already open), runs on the task executor, and opens the chosen one as a project tab.
- [ ] 7.2 Add a `Worktree: Remove…` command available on worktree project tabs. Use the existing dirty-prompt flow to close any dirty editor tabs first.
- [ ] 7.3 On clean worktree, call `RemoveWorktree(force=false)`. On the dirty signal, surface a confirmation prompt that names the worktree path and dirty file count; on confirm, retry with `force=true`.
- [ ] 7.4 If the worktree directory no longer exists, call `PruneWorktrees` and close the tab cleanly.
- [ ] 7.5 Refresh any open `Worktree: Open…` overlay after removal.

## 8. Validation

- [ ] 8.1 Run `cmake --build build/microide` and `ctest --test-dir build/microide --output-on-failure`.
- [ ] 8.2 Capture `MICROIDE_TRACE_REDRAW` evidence covering the new label render and a HEAD-change-driven label update; confirm only the affected tab area redraws.
- [ ] 8.3 Capture `MICROIDE_STARTUP_TRACE` covering session restore with a worktree project tab open; confirm cached label is shown without a synchronous git query and async resolve does not regress startup.
- [ ] 8.4 Update `docs/active-work.md` to add the worktree workflow to the shipped baseline; add a short `docs/worktrees.md` describing behavior, defaults, and limits.
