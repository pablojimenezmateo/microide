## ADDED Requirements

### Requirement: File-Finder Open Latency Budget

Opening the file-finder overlay over an indexed project SHALL complete within a documented budget on the reference host. The budget is measured from the user action that opens the overlay to the time the first set of results is visible in the UI. Changes that affect the file-finder open path SHALL include `MICROIDE_PERF_TRACE` before-and-after output and a green `file_finder_cold` harness run.

#### Scenario: File finder cold open on indexed project
- **WHEN** the user opens the file finder overlay over a project whose in-process index is fully built
- **THEN** the overlay SHALL display the initial result set within 50 ms on the reference host

#### Scenario: File finder open during initial index build
- **WHEN** the user opens the file finder overlay while the initial project index is still being built on the background thread
- **THEN** the overlay SHALL open immediately and display partial results from the portion of the index built so far, without blocking on index completion

### Requirement: Git Sidebar First-Paint Latency Budget

Activating the git sidebar SHALL display the first visible git-status result within a documented budget, measured from the sidebar activation event to the first rendered frame showing git-status data. The main thread SHALL NOT block during this interval.

#### Scenario: Git sidebar first paint after activation
- **WHEN** the user activates the git sidebar for a project with a git repository
- **THEN** the sidebar SHALL show git-status data within 200 ms on the reference host on a project with up to 10 000 tracked files, measured by the `git_sidebar_activate` harness scenario

#### Scenario: Git sidebar shows refreshing state immediately
- **WHEN** the git status subprocess is dispatched to the background executor
- **THEN** the sidebar SHALL display a "refreshing" visual state on the same frame that the activation event is processed, before any subprocess result is available

### Requirement: Search Time-To-First-Result Budget

Project-wide text search SHALL deliver the first batch of matching results to the UI within a documented budget, regardless of total corpus size. The budget is measured from search initiation to the first frame showing at least one result (or an empty-results indicator if the entire corpus has no match).

#### Scenario: Search time-to-first-result on a 10 000-file project
- **WHEN** the user initiates a project search on a corpus of 10 000 files with at least one matching file
- **THEN** the first result batch SHALL be visible in the UI within 100 ms on the reference host, measured by the `search_first_result` harness scenario

#### Scenario: Search time-to-first-result with no matches
- **WHEN** the user initiates a search with a pattern that matches no files in the project
- **THEN** the empty-results state SHALL appear within 100 ms on a 10 000-file project on the reference host

### Requirement: Idle CPU Budget After Background Work Settles

After all startup-triggered background work (file-index build, plugin asset monitor arm, git sidebar refresh, LSP initialise) has completed and the user has not interacted for 30 seconds, the application SHALL consume near-zero CPU and the event loop SHALL be parked. This requirement extends the existing Idle CPU Budget requirement to include the new background services introduced by this change.

#### Scenario: Idle after file-index build completes
- **WHEN** the file-index watcher has finished the initial build and no user interaction has occurred for 30 seconds
- **THEN** the process SHALL consume near-zero CPU and the event loop SHALL be parked on `SDL_WaitEvent`, as confirmed by the `idle_soak_30s` harness scenario

#### Scenario: Idle after git sidebar refresh settles
- **WHEN** a git status result has been delivered to the sidebar and no further git operations are pending
- **THEN** the git executor thread SHALL be blocked on its work queue and SHALL NOT produce periodic wake events
