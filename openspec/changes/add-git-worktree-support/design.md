## Context

MicroIDE already supports multiple open projects as tabs via `OpenProjectTab(project_root, …)` and `ProjectCatalogState`, with project-local persistence. `GitRepository` (in `src/project/`) wraps shell-level git execution and exposes status, working-tree entries, file history, and stage/discard primitives. There is no branch resolution helper, no worktree primitive, and no branch-aware label composition: `WorkspaceProjectPresentation` produces project labels solely from the directory name and accent.

`git worktree` is the canonical way to keep multiple branches of the same repository in independent working trees that share one `.git` object database. Creating a worktree is `git worktree add <path> [-b <newbranch>] [<startpoint>]`. Listing is `git worktree list --porcelain`. Removal is `git worktree remove <path>` (refuses if dirty without `--force`). After external deletion, `git worktree prune` cleans the administrative records.

Constraints: priority order is correctness > speed > low CPU > low memory. The change must not block the UI thread on git execution. Plugin Lua API is unaffected. Persistence format may grow optional fields without a major-version bump.

## Goals / Non-Goals

**Goals:**

- Users can create a worktree of the currently-open git project from a single command, with a guided prompt covering base branch, new-vs-existing branch, branch name, and target directory.
- Users can list and open existing worktrees of the active repository as new project tabs.
- Project tabs, breadcrumbs, and tooltips show `<project-name> · <branch>` for git-backed projects; the branch tracks `HEAD` updates without manual refresh.
- Session restore reopens worktree-backed projects with correct labels and origin metadata.
- A worktree project tab can be closed and removed (with `--force` confirmation when dirty, prune fallback when the directory is gone).
- Failures (existing worktree for branch, dirty branch, missing base, bare repo) surface as clear inline prompt errors, not silent no-ops.

**Non-Goals:**

- Managing remote tracking branches, fetch, push, or upstream changes from inside the worktree create flow. Use the existing git sidebar or a terminal.
- Migrating existing non-worktree project tabs into worktrees, or implicitly converting bare clones.
- Any UI for branching that is not coupled to worktree creation. Plain `git checkout -b` is out of scope; users still use a terminal for that.
- Submodule worktree handling, sparse-checkout configuration, or `git worktree move`. Out of scope for this change.

## Decisions

### D1. New `GitWorktreeService` rather than extending `GitRepository`

A new `src/project/GitWorktreeService.{h,cpp}` owns the worktree subcommands and result types. `GitRepository` gains only a `ResolveBranch()` helper (current `HEAD` short-form: branch name or `HEAD@<short-sha>` for detached). Keeping worktree logic in its own translation unit matches the existing one-service-per-concern pattern (`GitStatusService`, `GitCompareService`, `GitBlameService`) and keeps `GitRepository` focused on status and working-tree I/O.

`GitWorktreeService` exposes:

```cpp
struct WorktreeEntry {
  std::filesystem::path path;
  std::string branch;             // empty if detached
  std::string head_sha;
  bool is_main = false;
  bool is_detached = false;
  bool is_locked = false;
  bool is_prunable = false;
};

struct CreateWorktreeRequest {
  std::filesystem::path origin_root;
  std::filesystem::path target_path;
  std::string branch_name;        // required
  bool create_branch = true;      // false = checkout existing branch
  std::string base_revision;      // base branch or commit; "" = current HEAD
  bool force = false;
};

struct CreateWorktreeResult {
  bool ok = false;
  std::filesystem::path created_path;
  std::string error_message;
};

CreateWorktreeResult CreateWorktree(const CreateWorktreeRequest&);
std::vector<WorktreeEntry> ListWorktrees(const std::filesystem::path& origin_root);
bool RemoveWorktree(const std::filesystem::path& origin_root,
                    const std::filesystem::path& worktree_path,
                    bool force,
                    std::string* error_message);
bool PruneWorktrees(const std::filesystem::path& origin_root);
```

All methods run synchronously but are invoked off the UI thread by the workspace coordinator (existing `host-services` task executor), with results posted back to the main loop.

**Alternative considered:** linking against libgit2. Rejected — the project already commits to subprocess-based git execution everywhere else; adding a runtime dependency for worktree alone would be inconsistent and increases binary size.

### D2. Default worktree location: sibling `<repo>.worktrees/<branch>` directory

When the user does not override the target path, the prompt defaults to `<origin_parent>/<origin_basename>.worktrees/<sanitized-branch>`. This keeps worktrees grouped per repo without polluting the repo tree, avoids confusing the project's own file watcher (the worktree is not under the repo root), and is easy to clean up. The directory is auto-created if missing. Path sanitization replaces `/`, `\`, and OS-reserved characters with `-`.

**Alternative considered:** placing worktrees under `<repo>/.worktrees/<branch>`. Rejected because that path is inside the original working tree, which makes `.gitignore` semantics ambiguous and confuses the project tree view.

### D3. Prompt flow uses the existing `PromptSurfaceService` with a multi-step guided form

The "Create worktree" command opens a single command-prompt sequence with four ordered steps:

1. **Base** — defaults to current `HEAD`; the prompt offers a typeahead over branches (`git for-each-ref refs/heads --format=%(refname:short)`).
2. **Branch name** — required; the prompt validates that the name is a valid ref and either does not exist (then `-b` is added) or exists and is not currently checked out anywhere (then we omit `-b`).
3. **Target path** — defaults per D2; user can override.
4. **Confirm** — shows the resolved `git worktree add` invocation; submit triggers the call.

Errors from `git worktree add` (e.g., "branch already checked out", "fatal: invalid reference") render in the prompt's inline-error slot; the prompt stays open so the user can correct the input.

### D4. Tab metadata extension

`TabEntry` (project-tab variant) and `ProjectWorkspaceState` gain:

```cpp
struct ProjectGitMetadata {
  bool is_worktree = false;
  std::filesystem::path origin_root;   // empty when not a worktree
  std::string branch;                  // resolved branch or "HEAD@<sha>"
  std::int64_t branch_resolved_at = 0; // mtime-of-HEAD timestamp for cache freshness
};
```

`origin_root` is the worktree's origin repository root (i.e., the repo whose `.git` directory is shared). Detected at open time by reading `<worktree>/.git` (which for a worktree is a file, not a directory, pointing back at `<origin>/.git/worktrees/<name>`). For non-worktree repos, `origin_root` equals the project root; `is_worktree = false`.

### D5. Branch resolution and refresh

`GitRepository::ResolveBranch()` runs `git rev-parse --abbrev-ref HEAD` (returns the branch name, or `HEAD` for detached). On detached, falls back to `git rev-parse --short HEAD` and returns `HEAD@<short>`. Result is cached in `ProjectGitMetadata::branch` keyed by mtime of `<repo>/.git/HEAD` (for normal repos) or `<origin>/.git/worktrees/<name>/HEAD` (for worktrees). The host-owned tree watcher already watches the project root; we add a narrow watch for the appropriate `HEAD` file path so commits, checkouts, and rebases trigger a re-resolve and a single tab-label invalidate. No periodic polling.

### D6. Label composition

`WorkspaceProjectPresentation` gains:

```cpp
std::string ProjectTabLabel(const ProjectWorkspaceState&); // "<name> · <branch>" if branch known, else "<name>"
std::string ProjectBreadcrumbLabel(const ProjectWorkspaceState&);
std::string ProjectTooltipLabel(const ProjectWorkspaceState&);
```

The middle dot `·` separator and the order (name first, branch after) are fixed. The branch is truncated using the existing text-renderer truncation if the available tab width is too narrow; the project name is preserved at the cost of branch truncation. Tooltip always shows the full untruncated form plus the worktree origin path on a second line when `is_worktree` is true.

### D7. Persistence

`PersistedProjectTabState` gains optional `worktree_origin` (string path) and `branch_cache` (string) fields. Writers always emit them when `is_worktree` is true; readers tolerate their absence (older files without these fields are interpreted as `is_worktree = false` with no cached branch). On session restore, the cached branch is shown immediately; a background `ResolveBranch()` runs at project-open and updates the label if it disagrees with the cache.

### D8. Removal flow

The "Remove worktree" command is offered only for tabs where `is_worktree` is true. On invocation:

1. Close the project tab (using existing dirty-prompt flow if dirty editor tabs exist).
2. Call `RemoveWorktree(origin_root, worktree_path, force=false, …)`.
3. If git refuses because the worktree is dirty, surface a confirmation prompt (`Force remove worktree?`); on confirm, retry with `force=true`.
4. If the directory no longer exists on disk, call `PruneWorktrees(origin_root)`.
5. Refresh the worktree list shown in any open "Open worktree…" overlay.

### D9. File-watcher isolation

Worktree directories live outside the origin repo. The existing per-project file watcher already scopes to the project root, so opening a worktree as its own project tab gives it its own watcher without modification. The origin repo's project tab does not see worktree files, and vice versa. The only added watch is the `HEAD` file under each project's appropriate git directory.

## Risks / Trade-offs

- [Risk] Creating a worktree on a branch already checked out elsewhere fails → Mitigation: pre-validate in the prompt by querying `ListWorktrees()` and showing existing worktrees inline; offer "Open existing worktree" as a one-click pivot.
- [Risk] Default `<repo>.worktrees/` location collides with user files → Mitigation: prompt always shows the target path before confirm; user can override; auto-create only if the directory is empty or missing.
- [Risk] Detached `HEAD` after rebase/bisect mid-session shows a confusing tab label → Mitigation: detached state renders as `HEAD@<short>`, distinguishable from any branch name; tooltip clarifies "(detached HEAD)".
- [Risk] `HEAD` file watch fires too often during multi-step git operations (rebase, interactive rebase) → Mitigation: debounce by 100 ms in the watcher callback before re-resolving; only update the label if the resolved string actually changed.
- [Trade-off] Branch label adds visual noise to non-worktree projects → accepted; users opting into git-backed projects benefit from the branch always being visible. A future preference can disable it if users complain.
- [Risk] `git worktree remove --force` discards uncommitted work in the worktree → Mitigation: the confirmation prompt explicitly names the worktree path and lists dirty file count; the action is reversible only via reflog of the deleted branch's commits.
- [Trade-off] No `git worktree move` support → accepted; users rarely move worktrees; can do it via terminal and reopen.

## Migration Plan

No migration is required. Persistence-format additions are optional fields; existing project state files load unchanged. New behavior activates on the first launch with this change present.

## Open Questions

- Should the "Create worktree" command be available from the menu bar, the command palette, or both? Current lean: both — menu under `Project → New Worktree…`, plus command palette entry `Worktree: Create…`.
- Should a sibling `Worktree: Open…` command also offer worktrees from non-active project tabs (e.g., via a "switch repository" sub-prompt)? Current lean: no for v1; only the active project's worktrees are listed. Multi-repo worktree management can come later if needed.
- Should the tab label use `·` (middle dot), `:`, `@`, or a parenthesized branch like `name (branch)`? Current lean: `·` middle dot — minimal width cost, common convention in similar IDEs, no parser ambiguity in tooltips.
