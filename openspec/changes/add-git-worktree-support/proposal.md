## Why

Multi-branch work today forces users out of MicroIDE: switching branches mutates the working tree of the open project, and there is no way to keep two branches of the same repo open side by side as project tabs. Git supports this natively through `git worktree`, and MicroIDE's existing project-tab model already supports multiple project roots — the missing pieces are first-class worktree creation, opening a worktree as a new project tab, and tab labels that disambiguate worktrees of the same repository by branch.

## What Changes

- Add a "Create worktree" command on the active git-backed project that prompts for the base branch (existing branch or current `HEAD`), the new branch name (with an option to reuse an existing branch), and the worktree directory location (with a sensible default under a sibling `<repo>.worktrees/<branch>` path), then runs `git worktree add` and opens the resulting directory as a new project tab.
- Add a "Open worktree…" command that lists existing worktrees of the active repository (via `git worktree list --porcelain`) and opens the chosen one as a new project tab.
- Add tracked metadata on each project tab indicating whether it is a worktree, the originating repository, and the resolved branch (or detached `HEAD` short SHA).
- Update project tab titles, project breadcrumb labels, and project tooltip labels to render `<project-name> · <branch>` whenever the project is a git-backed root, so tabs from different worktrees of the same repository are visually distinguishable. Non-git projects continue to render the bare project name.
- Track branch changes asynchronously: when `HEAD` changes inside an open project (commit, checkout, rebase), the tab label updates without requiring a manual refresh.
- Add a "Remove worktree" command on a worktree project tab that closes the project tab, runs `git worktree remove` on the originating repository (with a `--force` confirmation flow if the worktree is dirty), and falls back to `git worktree prune` if the directory was already deleted out-of-band.
- Persist worktree-aware project tab metadata so session restore correctly reopens worktrees and labels them on startup without needing an immediate git query.
- Add focused fixture coverage for worktree creation, opening, tab labeling, branch-change refresh, and removal flows.

## Capabilities

### New Capabilities

- `git-worktrees`: durable contract for native worktree creation, opening, labeling, branch-tracking, persistence, and removal in MicroIDE's project-tab model.

### Modified Capabilities

- None. Project-tab labeling has no spec today; the new `git-worktrees` capability owns the worktree-aware label rules.

## Impact

- Affected code: `src/project/GitRepository.{h,cpp}` (worktree subcommand wrappers, branch resolution, worktree listing), new `src/project/GitWorktreeService.{h,cpp}` for create/list/remove flows, `src/workspace/WorkspaceProjectPresentation.{h,cpp}` (label composition with branch), `src/workspace/WorkspaceShell.h`/related coordinators (new commands and command-prompt flows), `src/workspace/WorkspacePersistenceFormat.{h,cpp}` (worktree metadata fields on persisted project tab state), and the command/menu registries that surface the new actions.
- Affected on-disk artifacts: persisted project state gains optional `worktree_origin`, `branch` cached fields. Backward-compatible with current files; no migration required.
- Affected docs: `docs/active-work.md` (shipped baseline updated), `docs/implementation-guide.md` (project tab model section), and a new short note in `docs/` covering worktree behavior and limits.
- Affected tests: new `microide_tests` fixtures covering `GitWorktreeService` (create with new branch, create against existing branch, list, remove, prune-after-external-delete), tab-label composition, branch-change refresh, and session-restore round-trip.
- Risk: low-medium — `git worktree` requires a non-bare repository and forbids creating two worktrees of the same branch; surface those failures clearly in the prompt flow rather than silently. Worktree directories created outside the original repo's tree must be tracked carefully so they don't appear inside the project's own filesystem watcher.
