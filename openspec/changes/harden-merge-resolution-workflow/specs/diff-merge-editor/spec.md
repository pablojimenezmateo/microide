## ADDED Requirements

### Requirement: Merge Conflicts Are Classified
Three-way merge tabs SHALL classify each conflict using explicit conflict kinds including both-modified, add/add, delete/modify, rename/rename, rename/delete, file/directory, binary, submodule, mode, and line-ending or whitespace-heavy text conflicts when Git state exposes enough information.

#### Scenario: Delete modify conflict
- **WHEN** one side deletes a file and the other side modifies it
- **THEN** the merge resolver SHALL identify the conflict as delete/modify and SHALL require the user to choose whether the result file exists

#### Scenario: Binary conflict
- **WHEN** Git reports a binary conflict
- **THEN** the merge resolver SHALL show a binary conflict summary and SHALL NOT present text hunk controls that cannot apply

### Requirement: Merge Resolver Labels Are Unambiguous
Merge tabs SHALL display unmistakable labels for current/ours, result, incoming/theirs, and base/common ancestor when base is available. Labels SHALL include branch or ref context where known and SHALL NOT rely on color alone.

#### Scenario: Merge tab opens from conflict row
- **WHEN** a conflict file opens in the merge resolver
- **THEN** the resolver SHALL label the current, result, incoming, and base contexts before the user selects any hunk

#### Scenario: Base pane toggled
- **WHEN** the user toggles the base pane
- **THEN** the merge tab SHALL show or hide the base/common ancestor pane without losing selected conflict, scroll position, or result dirty state

### Requirement: Merge Resolver Provides Required Actions
For text conflicts, merge tabs SHALL support accept current, accept incoming, accept both current first, accept both incoming first, edit result manually, reset result hunk, jump to next unresolved conflict, mark file resolved, reopen result file, show raw conflict markers, and copy branch-side snippet.

#### Scenario: Accept both current first
- **WHEN** a text conflict is selected and the user accepts both with current first
- **THEN** the result hunk SHALL contain current-side text followed by incoming-side text and the conflict SHALL be marked resolved

#### Scenario: Reset result hunk
- **WHEN** the user resets a resolved hunk
- **THEN** the result hunk SHALL return to its initial unresolved or auto-picked state and the remaining conflict count SHALL update

### Requirement: Merge Result State Is Visible
Merge tabs SHALL show file path, selected conflict index, total conflict count, remaining unresolved conflict count, remaining conflicted file count when known, and result state as dirty, saved, invalid, or resolved.

#### Scenario: Result edited manually
- **WHEN** the user edits the result pane manually
- **THEN** the merge tab SHALL mark the result dirty and SHALL not claim the file is resolved until validation succeeds

### Requirement: Mark Resolved Is Validated
Before a merge file is marked resolved, MicroIDE SHALL verify that the result is saved, conflict markers are absent unless explicitly overridden, expected result file existence matches the conflict decision, external modifications have not invalidated the open result, the Git index still represents the same conflict generation, and line endings are preserved or intentionally normalized.

#### Scenario: Conflict markers remain
- **WHEN** the user attempts to mark a file resolved while conflict markers remain in the result
- **THEN** MicroIDE SHALL block the action, jump to the first marker when requested, and offer an explicit override path rather than silently marking resolved

#### Scenario: Index changed under resolver
- **WHEN** the Git index conflict state changes after the merge tab opens
- **THEN** mark-resolved SHALL fail with a stale conflict state and SHALL offer to refresh the resolver
