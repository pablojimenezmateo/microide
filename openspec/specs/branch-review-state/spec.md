# branch-review-state Specification

## Purpose

Define local-only branch review memory: reviewed files and hunks, optional notes, and
changed-since-reviewed markers for branch compare workflows. State is project-scoped and
rendered from prebuilt view-model fields.

## Requirements

### Requirement: Branch Review Tracks Reviewed Files And Hunks
MicroIDE SHALL support local branch-review state that records reviewed files and reviewed hunks for a repository/base/head review target. Review state SHALL distinguish reviewed, unreviewed, and changed-since-reviewed status.

#### Scenario: File marked reviewed
- **WHEN** the user marks a file reviewed in branch review mode
- **THEN** MicroIDE SHALL record the file review against the current repository, base ref, head identity, path, and diff generation

#### Scenario: Hunk changed after review
- **WHEN** a hunk was reviewed and later changes content or line identity
- **THEN** MicroIDE SHALL show the hunk or containing file as changed since reviewed

### Requirement: Review State Is Local Only
Branch review state SHALL be local project data. MicroIDE SHALL NOT require hosted provider auth, network calls, or pull-request metadata to mark files or hunks reviewed.

#### Scenario: Offline branch review
- **WHEN** the user reviews a local branch without network access
- **THEN** reviewed markers and notes SHALL continue to work

### Requirement: Review Notes Leave The Review As References
MicroIDE SHALL offer the notes of the review the active compare tab belongs to as text, one line per note ordered by path and then line: `path:start[-end]: text` for a hunk's note, naming the hunk's lines in the new file; `path (removed line[s] start[-end]): text` for a hunk that only removes lines; `path: text` for a file's note. Paths SHALL be relative to the repository, and a note's line breaks SHALL be flattened to spaces. `copy-review-notes` SHALL put that text on the clipboard and `term-insert-review-notes` SHALL type it into the running terminal as one paste. Both SHALL cover every file of the review, not only the active tab's, and SHALL be unavailable when the review has no notes.

#### Scenario: Notes on two files go back to the agent
- **WHEN** the user notes a hunk in each of two files of a review and runs `term-insert-review-notes`
- **THEN** the terminal receives two lines, each `path:line: note`, in path order, as one paste

### Requirement: Review Notes Attach To File Or Hunk Identity
MicroIDE SHALL allow optional local notes attached to reviewed files or hunks. Notes SHALL be associated with the same repository/base/head/path/hunk identity used for reviewed markers.

#### Scenario: Note survives tab close
- **WHEN** the user adds a note to a hunk, closes the compare tab, and reopens the same branch review
- **THEN** the note SHALL be restored for the matching hunk identity

### Requirement: Review State Can Be Cleared And Pruned
MicroIDE SHALL support clearing review state for the active branch-review target and pruning obsolete review records by repository and review-target identity using bounded retention policy.

#### Scenario: User clears active target review state
- **WHEN** the user requests clear-review-state for the active branch-review target
- **THEN** reviewed markers and notes for that target SHALL be removed without affecting unrelated targets

#### Scenario: Obsolete records are pruned
- **WHEN** persisted review records exceed configured retention bounds
- **THEN** MicroIDE SHALL prune oldest obsolete records by review-target key while preserving current target state

### Requirement: Review Markers Render From View Models
Review marker rendering SHALL consume prebuilt view-model fields. Render translation units SHALL NOT query persistence, Git state, or review services during paint.

#### Scenario: File list shows reviewed markers
- **WHEN** the branch review file list renders files with mixed review status
- **THEN** the renderer SHALL paint reviewed, unreviewed, and changed-since-reviewed indicators from row view-model fields
