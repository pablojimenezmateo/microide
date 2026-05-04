## ADDED Requirements

### Requirement: Git Operations Run On Background Threads

All git subprocess calls (status, blame, log, diff) SHALL be dispatched to a per-project single-thread executor and SHALL NOT block the main thread. Results SHALL be delivered to the UI via SDL user event after the subprocess completes. The main thread SHALL remain responsive to input and rendering while any git operation is in flight.

#### Scenario: Git sidebar activation does not block the main thread
- **WHEN** the user activates the git sidebar
- **THEN** the git status subprocess SHALL start on the background executor; the sidebar SHALL immediately enter a "refreshing" visual state; the main thread SHALL return to its event loop without waiting for the process to complete

#### Scenario: Git blame gutter does not block tab activation
- **WHEN** the user activates a tab for a file with git blame enabled
- **THEN** the blame subprocess SHALL be dispatched to the background executor without blocking the tab hydration or first render of the editor surface

#### Scenario: Project switch cancels in-flight git operations
- **WHEN** the user switches projects while a git status or blame call is in flight
- **THEN** the in-flight result SHALL be discarded on delivery; the new project's executor SHALL start with a clean queue; no result from the old project SHALL be applied to the new project's UI state

#### Scenario: Rapid blame requests do not saturate the executor
- **WHEN** the user scrolls a file quickly and triggers many consecutive blame requests
- **THEN** the blame dispatcher SHALL debounce requests and the executor SHALL process only the most recent pending blame request, discarding superseded ones

### Requirement: LSP Document-Open Notification Is Non-Blocking

Sending `textDocument/didOpen` and `textDocument/didChange` notifications to an LSP server SHALL NOT block main-thread tab hydration or rendering. These notifications SHALL be dispatched asynchronously and SHALL NOT gate the display of the editor surface.

#### Scenario: Tab opens without waiting for LSP acknowledgement
- **WHEN** the user opens a file in a project with an active LSP server
- **THEN** the editor tab SHALL display the file contents and accept input before the LSP server has acknowledged `textDocument/didOpen`

#### Scenario: LSP server startup does not delay tab open
- **WHEN** the LSP server for the active language is still initialising
- **THEN** opening a tab SHALL NOT wait for the server to reach the `initialized` state before displaying the file

### Requirement: Architectural Lint Enforces Main-Thread Non-Blocking

The architectural-lint test SHALL hard-fail if any source file under `src/workspace/` calls a synchronous subprocess-wait primitive directly from a render or event-handler path. This prevents future contributors from reintroducing blocking patterns that were previously identified as regressions.

#### Scenario: Lint catches direct synchronous process wait in workspace code
- **WHEN** a workspace source file introduces a direct call to `Subprocess::Wait()`, `waitpid()`, or an equivalent blocking-wait primitive outside the designated background executor
- **THEN** `tests/ArchitectureInvariantsTests.cpp` SHALL hard-fail with the offending file and line number

#### Scenario: Lint passes for executor-dispatched subprocess calls
- **WHEN** a subprocess call is dispatched through the per-project single-thread executor and delivers results via SDL wake event
- **THEN** the lint SHALL pass without modification
