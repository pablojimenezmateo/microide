## ADDED Requirements

### Requirement: Always-Current In-Process Project File Index

MicroIDE SHALL maintain an always-current in-process index of all files under the active project root, updated by platform-native file-system events (inotify on Linux, FSEvents on macOS, ReadDirectoryChangesW on Windows) without polling the directory tree on demand. The index SHALL be populated on a background thread and SHALL NOT block the main thread during construction or update.

#### Scenario: File finder opens with no scan latency
- **WHEN** the user opens the file finder overlay
- **THEN** the overlay SHALL query the in-process index and display results immediately, without triggering a directory traversal or subprocess call

#### Scenario: File created in project directory appears in index
- **WHEN** a file is created under the active project root by an external tool or the host OS
- **THEN** the in-process index SHALL reflect the new file within the platform-native notification window (at most 500 ms on supported platforms)

#### Scenario: Initial index build does not stall startup
- **WHEN** MicroIDE opens a project for the first time or after a cold start
- **THEN** the initial index build SHALL run on a background thread; the application SHALL be interactive before the index is fully ready, and the file finder SHALL show partial results from whatever portion of the index has been built

#### Scenario: Native-event fallback on unsupported platforms
- **WHEN** the platform does not support native file-system events or the watch limit is exhausted (e.g., inotify `max_user_watches` reached)
- **THEN** the watcher SHALL fall back to a snapshot-diff poll at the same interval used by the existing plugin asset monitor, and SHALL log a one-time warning identifying the fallback reason

### Requirement: Index Is The Single Source Of Truth For File Discovery

File finder, project search, and diagnostic file resolution SHALL consume the in-process index directly and SHALL NOT trigger directory traversal or subprocess invocation to enumerate project files. The index SHALL be the sole authoritative source for all operations that require knowing which files exist under the project root.

#### Scenario: Project search reads from index, not from disk scan
- **WHEN** the user initiates a project-wide text search
- **THEN** the search worker SHALL obtain the list of files to scan from the in-process index without invoking any directory-traversal call at search time

#### Scenario: Project switch resets the index to the new root
- **WHEN** the user switches to a different project
- **THEN** the watcher SHALL detach from the old project root and begin building the index for the new root; the file finder and search SHALL reflect the new project's files after the index signals readiness
