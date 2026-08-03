## Purpose

Define how external repository and filesystem changes fan out into typed host events that refresh
Git state, editor buffers, compare tabs, and related surfaces without stale snapshots.

## Requirements

### Requirement: External Changes Fan Out Through Typed Events
MicroIDE SHALL normalize native file-watch and repository-change signals into typed project-relative events before updating Git state, editor buffers, compare tabs, merge tabs, file tree, search index, diagnostics, blame, commit draft state, or language-server document state.

#### Scenario: File changed outside app
- **WHEN** a watched project file is modified by another process
- **THEN** MicroIDE SHALL publish a typed project-file-changed event and SHALL NOT require each surface to poll the filesystem independently

#### Scenario: Branch changed outside app
- **WHEN** `.git/HEAD` or equivalent repository metadata changes outside MicroIDE
- **THEN** MicroIDE SHALL mark the repository snapshot stale and schedule an asynchronous refresh

### Requirement: Language Servers Are Told About On-Disk Changes
MicroIDE SHALL notify running language servers of project file changes it did not itself write, through `workspace/didChangeWatchedFiles`, so that a server's index does not diverge from disk for files the user has not opened. MicroIDE SHALL advertise `workspace.didChangeWatchedFiles.dynamicRegistration` and SHALL deliver only the changes matching the globs a server registered, rather than broadcasting every change to every server.

A file that is open in an editor buffer is synchronized through the `textDocument/did*` notifications and its content is client-owned until `textDocument/didClose`; the watched-file event is additive and SHALL NOT be treated as authority over an open document's content.

#### Scenario: Branch switch changes files that are not open
- **WHEN** a Git operation (switch, pull, stash pop, discard) rewrites project files that no editor tab has open
- **THEN** MicroIDE SHALL send `workspace/didChangeWatchedFiles` to every running server whose registered globs match those paths, so the server re-reads them instead of answering from a pre-change index

#### Scenario: Server registered no watchers
- **WHEN** a project file changes and no running language server has registered a file watcher
- **THEN** MicroIDE SHALL send no watched-file notification and SHALL NOT perform per-file URI or glob work for that batch

#### Scenario: Server registration is bounded
- **WHEN** a language server registers file watchers whose glob patterns expand without practical bound
- **THEN** MicroIDE SHALL cap the retained registrations and per-registration patterns rather than admit an unbounded per-file match cost on the shell thread

### Requirement: Dirty Buffers Are Protected
External file changes SHALL NOT silently overwrite dirty editor, compare-right, or merge-result buffers. Clean buffers MAY reload automatically according to configuration, but dirty buffers SHALL show a conflict prompt or stale state until the user chooses an action.

#### Scenario: Clean buffer changed externally
- **WHEN** an open clean editor buffer changes on disk
- **THEN** MicroIDE SHALL reload it or prompt according to configuration and SHALL preserve caret/scroll anchors where safe

#### Scenario: Dirty buffer changed externally
- **WHEN** an open dirty buffer changes on disk
- **THEN** MicroIDE SHALL keep the in-memory edits, show an external-change conflict prompt, and SHALL NOT overwrite either version silently

### Requirement: Diff Tabs Refresh Safely
Open compare tabs SHALL become stale when their working-tree side, commit-side path mapping, branch target, or repository snapshot generation changes. Refresh SHALL run asynchronously and preserve selected hunk and scroll anchors where valid.

#### Scenario: Diff target file changes
- **WHEN** the working-tree file displayed in a compare tab changes externally
- **THEN** the compare tab SHALL show stale or refreshing state until the model refresh completes

#### Scenario: Diff target file is renamed
- **WHEN** the working-tree file for an open compare tab is renamed externally and the repository snapshot can identify the rename
- **THEN** the compare tab SHALL retarget the working-tree side while preserving the original commit-side path

### Requirement: Merge Tabs Invalidate Unsafe Resolved State
Open merge tabs SHALL invalidate resolved state when the result file, conflict index state, branch state, or source conflict files change externally. Mark-resolved SHALL require revalidation after such events.

#### Scenario: Merge result changed externally
- **WHEN** the result file of an open merge tab is modified outside MicroIDE
- **THEN** the merge tab SHALL mark its result state stale or dirty and SHALL require validation before mark-resolved can succeed

#### Scenario: Conflict index changed externally
- **WHEN** another Git command stages or resolves the same conflicted file
- **THEN** MicroIDE SHALL prevent the stale merge tab from marking the old conflict resolved

### Requirement: Watcher Bursts Are Coalesced
File-watch bursts SHALL be coalesced before triggering expensive Git, search, diagnostics, blame, compare, or merge refresh work. After burst processing settles and no user-visible work remains, MicroIDE SHALL return to the idle CPU budget.

#### Scenario: Build tool rewrites many files
- **WHEN** a build tool modifies many files rapidly
- **THEN** MicroIDE SHALL coalesce refresh work, keep the UI responsive, and avoid one Git subprocess per watcher event
