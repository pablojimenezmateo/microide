## ADDED Requirements

### Requirement: Git Sidebar Is Grouped By Workflow State
The Git sidebar SHALL present repository state as branch/upstream summary followed by grouped sections for Conflicts, Staged, Changed, Untracked, and Outgoing entries when those sections contain data.

#### Scenario: Repository has conflicts and staged files
- **WHEN** the repository snapshot contains unmerged entries and staged entries
- **THEN** the Git sidebar SHALL render separate Conflicts and Staged sections with counts and SHALL NOT flatten both into one changed-files list

#### Scenario: Repository snapshot is stale
- **WHEN** the current repository snapshot is marked stale
- **THEN** the Git sidebar SHALL show that refresh is pending or needed while continuing to display the last known rows

### Requirement: Git Rows Expose Predictable Actions
Each Git sidebar row SHALL expose a typed action set based on row kind. Keyboard shortcuts SHALL include `Enter` for the best default view, `d` for diff where available, `s` for stage, `u` for unstage, `x` for discard with confirmation, `m` for merge resolver where available, `c` for commit staged changes, `r` for refresh, and `o` for the working-tree file where available.

#### Scenario: Conflict row default action
- **WHEN** a conflict row is selected and the user presses `Enter`
- **THEN** MicroIDE SHALL open the merge resolver for that file rather than a plain editor tab

#### Scenario: Changed row diff action
- **WHEN** a modified unstaged row is selected and the user presses `d`
- **THEN** MicroIDE SHALL open a working-tree diff for that file

#### Scenario: Staged row unstage action
- **WHEN** a staged row is selected and the user presses `u`
- **THEN** MicroIDE SHALL dispatch an unstage operation for that file and refresh repository state after completion

### Requirement: Destructive Actions Are Previewed
Git sidebar discard actions SHALL require an explicit preview or summary before data is removed from the worktree or index. Confirmation SHALL identify the target path, operation kind, and whether staged, unstaged, or untracked data will be affected.

#### Scenario: Discard modified file
- **WHEN** the user requests discard for a modified file from the Git sidebar
- **THEN** MicroIDE SHALL show the diff or a clear summary of changes that will be lost before enabling confirmation

#### Scenario: Discard untracked file
- **WHEN** the user requests discard for an untracked file
- **THEN** MicroIDE SHALL identify the file as untracked and require confirmation before moving it to trash or deleting it according to the existing file-operation policy

### Requirement: Git Sidebar Renders From View Models
The Git sidebar render path SHALL consume prebuilt section and row view models. It SHALL NOT parse Git status, assemble row labels from repository internals, or run filesystem/Git queries during paint.

#### Scenario: Sidebar repaints without state change
- **WHEN** the Git sidebar repaints with no repository snapshot, selection, or layout change
- **THEN** render code SHALL reuse prebuilt row text and action flags rather than materializing new Git labels
