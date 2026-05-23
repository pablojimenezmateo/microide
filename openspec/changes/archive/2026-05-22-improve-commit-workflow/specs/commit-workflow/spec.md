## ADDED Requirements

### Requirement: Commit Workflow Shows Staged Summary
MicroIDE SHALL provide a commit workflow surface that shows staged file count, added/deleted line summary when available, staged file list, branch identity, upstream/ahead/behind state when known, and access to the staged diff.

#### Scenario: Staged files exist
- **WHEN** the user opens the commit workflow with staged changes
- **THEN** MicroIDE SHALL show the staged summary and enable commit message entry

#### Scenario: No staged files exist
- **WHEN** the user opens the commit workflow with no staged changes
- **THEN** MicroIDE SHALL show that there is nothing staged and SHALL disable commit execution

### Requirement: Commit Drafts Are Repository-Scoped
Commit message subject and body drafts SHALL be persisted per repository and branch/head context through the existing persistence service. A successful commit SHALL clear the draft for the committed context.

#### Scenario: Draft survives restart
- **WHEN** the user types a commit message, exits MicroIDE, and reopens the same repository and branch
- **THEN** the commit workflow SHALL restore the draft message

#### Scenario: Commit succeeds
- **WHEN** a commit completes successfully
- **THEN** MicroIDE SHALL clear the draft associated with that commit context

### Requirement: Pre-Commit Checks Are Structured
Before executing a commit, MicroIDE SHALL run structured checks for empty subject, excessively long subject, unresolved conflicts, remaining conflict markers in staged text, unstaged changes in files that are also staged, branch behind/upstream warning when known, and untracked files not included. Blocking checks SHALL prevent commit; warning checks SHALL require acknowledgement.

#### Scenario: Empty subject
- **WHEN** the commit subject is empty
- **THEN** MicroIDE SHALL block commit execution and focus the subject field

#### Scenario: Staged file has unstaged leftovers
- **WHEN** a file has staged and unstaged changes
- **THEN** MicroIDE SHALL warn that the commit will include only the staged portion and require acknowledgement before committing

#### Scenario: Conflict marker is staged
- **WHEN** staged content still contains conflict markers
- **THEN** MicroIDE SHALL block the commit unless an explicit override policy is added in a later change

### Requirement: Commit Operations Are Async And Report Output
Commit, amend, and commit-without-hooks operations SHALL run outside the main thread. For the preview workflow, commit subprocess stdin SHALL be noninteractive by default. Results SHALL distinguish success, cancelled, hook failed, dirty worktree, conflict, auth failed, repo locked, and unknown failure. Hook output SHALL be visible in a native output panel or equivalent host-owned surface.

#### Scenario: Hook fails
- **WHEN** a commit hook exits non-zero
- **THEN** MicroIDE SHALL report that no commit was created, show hook output, and keep the commit draft intact

#### Scenario: Commit succeeds
- **WHEN** a commit operation succeeds
- **THEN** MicroIDE SHALL refresh repository state and show the new clean/ahead state when known

#### Scenario: Hook waits for stdin
- **WHEN** a commit hook waits for stdin while commit subprocess stdin is noninteractive
- **THEN** MicroIDE SHALL keep the UI responsive, surface hook output/progress, and allow cancellation where supported by the process seam

#### Scenario: Commit identity is not configured
- **WHEN** Git commit fails because author name or email is not configured
- **THEN** MicroIDE SHALL report a structured commit failure with actionable output details and SHALL keep the commit draft intact

#### Scenario: Commit signing fails
- **WHEN** commit creation fails due to signing configuration or signer availability
- **THEN** MicroIDE SHALL report a structured commit failure and SHALL not clear staged state or drafts

#### Scenario: Commit succeeds but refresh fails
- **WHEN** the commit command succeeds and repository refresh fails afterward
- **THEN** MicroIDE SHALL report commit success with a refresh warning state instead of reporting commit failure

### Requirement: Amend And No-Hook Actions Require Confirmation
Commit amend and commit-without-hooks actions SHALL require explicit confirmation before execution.

#### Scenario: Amend requested
- **WHEN** the user requests commit amend
- **THEN** MicroIDE SHALL explain that the previous commit will be rewritten and require confirmation

#### Scenario: Commit without hooks requested
- **WHEN** the user requests commit without hooks
- **THEN** MicroIDE SHALL explain that hooks will be bypassed and require confirmation
