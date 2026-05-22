## ADDED Requirements

### Requirement: Compare Review Modes Are Explicit
Compare tabs SHALL declare one of four review modes: working-tree review, commit review, branch review, or conflict review. Each mode SHALL expose mode-appropriate metadata, file lists, navigation, and actions without relying on label text to infer behavior.

#### Scenario: Working-tree review mode
- **WHEN** a user opens a modified file from the Changed section
- **THEN** the compare tab SHALL identify itself as working-tree review and SHALL distinguish unstaged, staged, and combined staged-plus-unstaged views when those targets are available

#### Scenario: Commit review mode
- **WHEN** a user opens a commit comparison
- **THEN** the compare tab SHALL show commit metadata, changed files, and parent/current file actions for the selected file

#### Scenario: Branch review mode
- **WHEN** a user reviews outgoing changes against a base branch
- **THEN** MicroIDE SHALL expose outgoing commits, aggregate changed files across the range, and per-file compare tabs associated with that base revision

### Requirement: Branch Review Target Identity Is Stable
Branch review mode SHALL use an explicit target identity composed of repository root, resolved base commit, resolved head commit, merge-base commit when applicable, and repository snapshot generation when worktree or index content contributes to the review.

#### Scenario: Rebase changes review identity
- **WHEN** the branch is rebased and resolved base/head commits change
- **THEN** MicroIDE SHALL treat the branch review target as a new identity and SHALL invalidate stale review-derived state tied to the previous identity

### Requirement: Diff Model And Presentation Are Separate
Compare behavior SHALL separate semantic diff data from visible presentation state. Semantic data SHALL include file changes, hunks, old and new line ranges, mode changes, rename/copy metadata, binary metadata, and submodule metadata. Presentation state SHALL include collapsed regions, visible rows, inline highlights, syntax tokens, selection mapping, and scroll markers.

#### Scenario: Renderer consumes presentation rows
- **WHEN** a compare surface paints visible rows
- **THEN** it SHALL consume presentation rows and decorations built before render and SHALL NOT inspect Git file-change metadata directly

#### Scenario: Whitespace visualization changes
- **WHEN** the user toggles whitespace visualization without changing ignore-whitespace mode
- **THEN** MicroIDE SHALL rebuild presentation decorations as needed without recomputing the semantic hunk model

### Requirement: Diff Review Presentation Details
Compare tabs SHALL support inline word diff inside changed lines, context expansion above and below hunks, collapsed unchanged regions with line counts, sticky hunk headers where layout permits, syntax highlighting on both sides, whitespace visualization, ignore-whitespace mode, and metadata rows for binary, rename, mode, line-ending, and submodule changes.

#### Scenario: Collapsed context is expanded
- **WHEN** the user expands context above a collapsed hunk
- **THEN** the compare tab SHALL reveal additional unchanged rows while preserving the selected hunk and corresponding file-line mapping

#### Scenario: Binary file is compared
- **WHEN** the selected file is binary
- **THEN** the compare tab SHALL show a binary-file summary and SHALL NOT attempt to render invalid text hunks

#### Scenario: Rename and mode change are compared
- **WHEN** a file was renamed and its executable bit changed
- **THEN** the compare tab SHALL show both the old-to-new path and mode-change metadata in the file summary

### Requirement: Diff Review Navigation And Copy Actions
Compare tabs SHALL support next/previous changed file, next/previous hunk, open working-tree file at corresponding line, copy file path, copy hunk as patch, and copy file patch actions where the underlying target supports them.

#### Scenario: Open corresponding line
- **WHEN** the user invokes open-file on a visible added line in a working-tree compare
- **THEN** MicroIDE SHALL open the working-tree file at the corresponding new-side line

#### Scenario: Copy hunk as patch
- **WHEN** a hunk is selected and the user invokes copy-hunk
- **THEN** MicroIDE SHALL place a patch for that hunk on the clipboard using repository-relative paths

### Requirement: Large Diff Correctness Is Preserved
Large diff handling SHALL preserve hunk correctness. MicroIDE MAY show progress, delay non-essential inline highlights, or use interaction throttles, but SHALL NOT truncate changed files, omit hunks, or switch to a coarser incorrect diff based only on file size.

#### Scenario: Huge branch diff opens
- **WHEN** branch review includes many changed files or a very large changed file
- **THEN** MicroIDE SHALL either show correct diff data or an explicit still-loading/error state and SHALL NOT present an incomplete diff as final
