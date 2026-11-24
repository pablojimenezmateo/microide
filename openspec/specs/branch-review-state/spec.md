# branch-review-state Specification

## Purpose

Define branch review memory: reviewed files and hunks, optional notes, and
changed-since-reviewed markers for branch compare workflows. State is project-scoped,
kept on the machine that made it, and rendered from prebuilt view-model fields; a remote
project's also follows the user to their other machine (remote-connection-lifecycle,
"A review follows the user between machines").

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
MicroIDE SHALL offer the notes of a review as text, one line per note ordered by path and then line: `path:start[-end]: text` for a hunk's note, naming the hunk's lines in the new file; `path (removed line[s] start[-end]): text` for a hunk that only removes lines; `path: text` for a file's note. Paths SHALL be relative to the repository, and a note's line breaks SHALL be flattened to spaces. `copy-review-comments [path]` SHALL put that text on the clipboard and `term-insert-review-comments [path]` SHALL type it into the running terminal as one paste. The review SHALL be the active compare tab's when it takes review marks, and otherwise the repository's review touched last. Both SHALL cover every file of the review, not only the active tab's, unless a path narrows them to that file or the files under that directory (matched by whole path components), and SHALL be unavailable when there are no such notes. The Git menu SHALL offer both, labelled with the note count, and the git view's context menu SHALL offer them for an entry, a directory, or the Outgoing header (the whole review).

#### Scenario: Notes on two files go back to the agent
- **WHEN** the user notes a hunk in each of two files of a review and runs `term-insert-review-comments`
- **THEN** the terminal receives two lines, each `path:line: note`, in path order, as one paste

#### Scenario: Notes are copied with no diff in front
- **WHEN** the user has noted hunks in a review, switches to an editor tab, and opens the Git menu
- **THEN** Copy Review Comments SHALL be enabled, show the note count, and copy that review's notes

### Requirement: Review Marks And Notes Are Made On The Diff
A compare tab that takes review marks SHALL show, on the first row of the hunk under the pointer or holding the selection, a button that marks it reviewed (or unmarks it when it is reviewed) and, while the hunk has no note, a Comment button; a marked hunk SHALL keep a badge naming its status on that row. A hunk's note SHALL show as a row under its last row (a file's note at the top) with Edit and Delete buttons. Comment and Edit SHALL open a one-line prompt seeded with the note that writes the note of the review, file and hunk identity the prompt was opened on, whatever the diff's selection or content does while it is open; an empty entry SHALL delete the note. The compare header SHALL show the review's reviewed-hunk count and a button that marks every hunk of the file reviewed (or unmarks them), and every compare tab with changes SHALL offer Previous Change and Next Change buttons. Buttons SHALL be laid out by one function each that paint, click, hover, cursor shape and the control channel's `ui` query share.

#### Scenario: A hunk is marked from the diff
- **WHEN** the user clicks Mark Reviewed on a hunk
- **THEN** the hunk SHALL be marked reviewed, its row SHALL show the Reviewed badge, and the header count SHALL include it

#### Scenario: A comment keeps its hunk while the diff moves
- **WHEN** the user opens Comment on one hunk and the selection moves to another before the comment is saved
- **THEN** the note SHALL be attached to the hunk the prompt was opened on

### Requirement: Review Notes Attach To File Or Hunk Identity
MicroIDE SHALL allow optional local notes attached to reviewed files or hunks. Notes SHALL be associated with the same repository/base/head/path/hunk identity used for reviewed markers.

#### Scenario: Note survives tab close
- **WHEN** the user adds a note to a hunk, closes the compare tab, and reopens the same branch review
- **THEN** the note SHALL be restored for the matching hunk identity

#### Scenario: A note stays in sight when its hunk changes
- **WHEN** the hunk a note was written on has different content at the same position
- **THEN** the note SHALL still show under that hunk, marked as written before the edit, and saving it SHALL re-attach it to the hunk as it stands without leaving a second note

#### Scenario: A note follows a hunk whose lines moved
- **WHEN** an edit elsewhere in the file moves a hunk's lines without changing its content
- **THEN** the hunk's note SHALL show under the hunk at its new position

#### Scenario: A note whose hunk is gone is detached, not hidden
- **WHEN** no hunk of the file's diff matches a hunk note by position or by content
- **THEN** the note SHALL show at the top of the file's diff with its line, SHALL remain editable and deletable there, and SHALL be counted and exported with the review's other notes

### Requirement: Marking A File Reviewed Marks Its Hunks In One Pass
Marking or unmarking a whole file SHALL record or remove every hunk of the file's diff and the file's own mark in a single pass over the review state (no per-hunk re-resolve of the file), and each hunk SHALL keep an entry of its own so a later edit shows as changed on the hunk it touched.

#### Scenario: A large file does not evict other files' marks
- **WHEN** a file with more hunks than the per-target hunk-entry bound is marked reviewed
- **THEN** every hunk of that file SHALL read reviewed, and hunks marked on their own in other files SHALL stay marked: past the bound, the entries dropped first SHALL be those of files marked whole, whose hunks fall back to the file's mark

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
