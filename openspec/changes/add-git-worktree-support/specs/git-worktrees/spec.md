## ADDED Requirements

### Requirement: Create Worktree From Active Project

MicroIDE SHALL provide a command that creates a new git worktree from the currently active git-backed project and opens the resulting directory as a new project tab. The command SHALL guide the user through choosing a base branch, a target branch (new or existing), and a target directory.

#### Scenario: Create with new branch from current HEAD
- **WHEN** the active project is a git repository, the user invokes the create-worktree command, accepts the default base (current `HEAD`), enters a branch name that does not yet exist, and accepts the default target path
- **THEN** MicroIDE SHALL run `git worktree add -b <branch> <target> HEAD`, open the resulting directory as a new project tab, and label the tab with the project name and the new branch

#### Scenario: Create on existing branch
- **WHEN** the user enters a branch name that already exists locally and is not currently checked out in any worktree
- **THEN** MicroIDE SHALL run `git worktree add <target> <branch>` without `-b`, open the resulting directory as a new project tab, and label it accordingly

#### Scenario: Branch already checked out in another worktree
- **WHEN** the user enters a branch that is already checked out in some worktree
- **THEN** the prompt SHALL render an inline error identifying the conflicting worktree path and SHALL offer a one-click pivot to open that existing worktree as a project tab instead

#### Scenario: Active project is not a git repository
- **WHEN** the active project has no `.git` directory or `.git` worktree pointer
- **THEN** the create-worktree command SHALL be unavailable (or SHALL fail with a clear inline message if invoked through a keybinding)

### Requirement: Open Existing Worktree

MicroIDE SHALL provide a command that lists existing worktrees of the active repository and opens a chosen one as a new project tab.

#### Scenario: List and open
- **WHEN** the user invokes the open-worktree command on a git-backed active project
- **THEN** MicroIDE SHALL list the worktrees reported by `git worktree list --porcelain` (excluding any already open as a project tab), and on selection SHALL open the chosen worktree as a new project tab

#### Scenario: All worktrees already open
- **WHEN** every worktree of the active repository is already open as a project tab
- **THEN** the command SHALL display a non-blocking message indicating there is nothing to open and SHALL not modify the project catalog

### Requirement: Worktree-Aware Project Tab Labels

Project tab titles, project breadcrumbs, and project tooltips SHALL render the resolved branch alongside the project name for any git-backed project, so worktrees of the same repository are visually distinguishable.

#### Scenario: Git-backed project label
- **WHEN** a project tab is rendered for a git-backed project with a resolvable branch
- **THEN** the tab title and breadcrumb SHALL display `<project-name> · <branch>` (with the project name preserved and the branch truncated when width is constrained), and the tooltip SHALL show the full untruncated label

#### Scenario: Detached HEAD
- **WHEN** the project's `HEAD` is detached
- **THEN** the label SHALL render the branch slot as `HEAD@<short-sha>` and the tooltip SHALL include `(detached HEAD)`

#### Scenario: Worktree tooltip identifies origin
- **WHEN** the project tab is a worktree
- **THEN** the tooltip SHALL include a second line showing the originating repository path

#### Scenario: Non-git project label
- **WHEN** the project has no resolvable git branch
- **THEN** the tab title, breadcrumb, and tooltip SHALL render the bare project name without a branch suffix

### Requirement: Branch Tracks HEAD Without Manual Refresh

The branch shown on a project tab SHALL update automatically when the project's `HEAD` changes (commit, checkout, rebase, reset) without requiring user action.

#### Scenario: Commit in active project
- **WHEN** a commit is made inside an open project tab
- **THEN** the tab label SHALL re-resolve the branch within a debounced window and SHALL update the rendered label only if the resolved string changed

#### Scenario: Checkout from terminal inside the project
- **WHEN** the user runs `git checkout <other-branch>` from a terminal inside the project
- **THEN** the tab label SHALL update to reflect the new branch without polling

### Requirement: Persisted Worktree Metadata Survives Session Restore

Worktree-aware project tab metadata SHALL be persisted with the project tab and SHALL be restored on application start without requiring a synchronous git query before showing the label.

#### Scenario: Restart with open worktree tab
- **WHEN** the user closes MicroIDE with one or more worktree project tabs open and reopens the application
- **THEN** each worktree tab SHALL be restored with the cached branch label visible immediately on startup, and an asynchronous resolve SHALL update the label if the underlying branch has changed

#### Scenario: Older project state files
- **WHEN** the application loads a project state file that predates this change and lacks worktree metadata
- **THEN** the tab SHALL load as a non-worktree project, the branch SHALL be resolved asynchronously, and persistence SHALL upgrade the file on next save

### Requirement: Remove Worktree Command

A worktree project tab SHALL offer a remove-worktree command that closes the tab and removes the worktree from the originating repository, with explicit confirmation when the worktree is dirty.

#### Scenario: Clean worktree removal
- **WHEN** the user invokes remove-worktree on a clean worktree project tab and confirms
- **THEN** MicroIDE SHALL close the project tab (using the existing dirty-prompt flow for any dirty editor tabs), run `git worktree remove <path>` against the originating repository, and SHALL refresh any open worktree-list overlay

#### Scenario: Dirty worktree force removal
- **WHEN** `git worktree remove` refuses because the worktree is dirty
- **THEN** MicroIDE SHALL surface a confirmation prompt that names the worktree path and the count of dirty files, and on confirm SHALL retry with `--force`

#### Scenario: Worktree directory deleted out-of-band
- **WHEN** the worktree directory no longer exists on disk at remove time
- **THEN** MicroIDE SHALL run `git worktree prune` against the originating repository and SHALL close the project tab cleanly

### Requirement: Default Worktree Location

The create-worktree prompt SHALL default the target path to a sibling directory grouped per repository, and SHALL surface the resolved path before confirmation.

#### Scenario: Default path
- **WHEN** the user opens the create-worktree prompt and does not override the target path
- **THEN** the proposed target SHALL be `<origin_parent>/<origin_basename>.worktrees/<sanitized-branch>`, with reserved characters in the branch name replaced by `-`, and the parent directory SHALL be auto-created if missing

#### Scenario: Path override
- **WHEN** the user types a different target path before confirming
- **THEN** MicroIDE SHALL use the typed path verbatim and SHALL fail with a clear inline error if the path already exists and is non-empty

### Requirement: Worktree Operations Run Off The UI Thread

Worktree create, list, remove, and prune operations SHALL run on a background task executor and SHALL NOT block the UI thread or stall the render or input hot paths.

#### Scenario: Slow git invocation
- **WHEN** a worktree command takes more than one frame to complete
- **THEN** the UI SHALL remain responsive (typing, scrolling, terminal output continue), the prompt SHALL show a progress indicator, and the result SHALL be posted back to the main loop on completion
