## ADDED Requirements

### Requirement: Authoritative Git Repository Snapshot
MicroIDE SHALL represent repository state for each open project as an immutable `GitRepositoryState` snapshot owned by a Git repository service. The snapshot SHALL include repository root, head identity, branch name, upstream identity when known, ahead and behind counts when known, staged entries, unstaged entries, untracked entries, conflict entries, submodule entries, ongoing operation state, refresh generation, refresh timestamp, stale flag, and last refresh error.

#### Scenario: Multiple surfaces read the same snapshot
- **WHEN** the Git sidebar, status bar, compare opener, and commit workflow request repository state for the same project generation
- **THEN** each surface SHALL receive data derived from the same `GitRepositoryState` snapshot and SHALL NOT issue independent status commands on the render or input path

#### Scenario: Snapshot exposes stale state
- **WHEN** a file watcher or external branch change marks repository state dirty while a refresh is still in flight
- **THEN** the current snapshot SHALL remain readable with its stale flag set until a newer generation is published

### Requirement: Machine-Stable Git Status Parsing
Git status snapshots SHALL be parsed from machine-oriented Git output. Path lists SHALL use NUL-delimited output, and status parsing SHALL treat staged, unstaged, untracked, conflicted, renamed, deleted, typechanged, ignored, binary-relevant, and submodule states as first-class values.

#### Scenario: Path contains spaces and quotes
- **WHEN** Git reports a changed path containing spaces, quotes, backslashes, or non-ASCII UTF-8 in NUL-delimited status output
- **THEN** MicroIDE SHALL preserve the exact repository-relative path bytes after UTF-8 validation and SHALL NOT apply shell-style unquoting

#### Scenario: Rename with edits is parsed
- **WHEN** Git reports a renamed file with content edits
- **THEN** the snapshot entry SHALL preserve old path, new path, rename status, and staged or unstaged modification state

#### Scenario: Conflict state is classified
- **WHEN** Git reports unmerged entries
- **THEN** the snapshot SHALL classify the conflict kind instead of flattening it into a generic modified file row

### Requirement: Async Refresh Generations
Git refresh work SHALL run outside the main thread through the project background executor or an equivalent background seam. Each refresh SHALL carry a monotonically increasing generation ID, and stale completions SHALL NOT replace newer snapshots.

#### Scenario: Slow refresh completes after a newer refresh
- **WHEN** refresh generation 10 is still running and generation 11 completes first
- **THEN** generation 11 SHALL remain the published snapshot and generation 10 SHALL be ignored when it later completes

#### Scenario: Refresh requests are coalesced
- **WHEN** many filesystem events arrive for the same repository before the current refresh completes
- **THEN** MicroIDE SHALL coalesce them into a bounded number of follow-up refreshes instead of spawning one Git subprocess per event

### Requirement: Structured Git Refresh Failure
Git refresh failures SHALL be represented as structured snapshot errors with a stable category and optional Git stderr detail. UI callers SHALL be able to distinguish at least `not_a_repo`, `repo_locked`, `cancelled`, `auth_failed`, `submodule_error`, and `unknown_error`.

#### Scenario: Repository lock blocks status
- **WHEN** Git status fails because the repository is locked by another Git operation
- **THEN** the service SHALL publish a failed refresh state with category `repo_locked` and SHALL retain the last successful snapshot as stale when available

#### Scenario: Project is not a repository
- **WHEN** an open project has no Git repository root
- **THEN** the service SHALL publish a `not_a_repo` state and Git UI surfaces SHALL render an explicit non-repository state instead of an empty successful status
