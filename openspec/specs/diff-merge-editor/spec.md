## Purpose

Define the durable behavior and performance contract for compare and three-way merge editor surfaces.
## Requirements
### Requirement: Unified Decorated Text-Grid Pipeline

Editor, compare, and three-way merge surfaces SHALL render through one shared decorated text-grid pipeline that owns row fills, syntax runs, selection, diagnostic underlines, blame shadow text, and hunk decorations. Compare and merge surfaces SHALL NOT fork the editor's render path to add their own row-decoration layers.

#### Scenario: Editor and compare share row-decoration build
- **WHEN** a compare tab and a normal editor tab render the same underlying file content at the same viewport width
- **THEN** both surfaces SHALL reuse the shared row-decoration build and text-grid paint, and diverge only in the hunk-marker and split-column overlays that are specific to compare

#### Scenario: Merge tab uses the same pipeline as compare
- **WHEN** a three-way merge tab renders its incoming, result, and current panes
- **THEN** each pane SHALL reuse the shared decorated text-grid pipeline, adding only the merge-specific hunk selection and choice markers on top

### Requirement: Diff Semantics Do Not Degrade By File Size

Diff and merge semantics SHALL NOT silently degrade, truncate, or fall back to coarser algorithms based on file-size thresholds. Performance optimizations MAY trade CPU for memory but SHALL NOT reduce the correctness of the displayed hunks.

#### Scenario: Large file opened in compare
- **WHEN** a compare tab is opened for a file that exceeds any large-file heuristic threshold
- **THEN** the compare tab SHALL compute and display hunks using the same algorithm used for small files, with no "diff disabled" or "diff truncated" fallback

#### Scenario: Merge conflict in a large file
- **WHEN** a three-way merge tab is opened against a file exceeding the large-file heuristic
- **THEN** every conflict hunk SHALL be resolvable through the normal per-hunk and whole-side apply actions, without the large-file path disabling merge controls

### Requirement: Compare Tab Behavior

Compare tabs SHALL support working-tree versus `HEAD`, working-tree versus arbitrary commit, working-tree versus base-branch `HEAD`, and commit-versus-commit compares. Each compare tab SHALL show a vertical change-overview lane mirroring the scrollbar and SHALL support per-hunk navigation with `[` and `]`.

#### Scenario: Navigating between hunks
- **WHEN** a compare tab is focused and the user presses `]`
- **THEN** the caret and viewport SHALL move to the next hunk boundary, wrapping or stopping at end of file according to the documented navigation rule

#### Scenario: Opening the working-tree file from a compare row
- **WHEN** the user presses `Enter` or `o` on a compare row
- **THEN** MicroIDE SHALL open the working-tree file at the corresponding line in a normal editor tab

### Requirement: Three-Way Merge Tab Behavior

Three-way merge tabs SHALL display incoming, result, and current panes, support per-hunk picks (incoming / base / current / both), support whole-side apply, provide a change-overview lane, and allow the merged result to be opened in a normal editor tab with `o`.

#### Scenario: Per-hunk apply
- **WHEN** a merge tab is focused, a hunk is selected, and the user presses `i`, `b`, `c`, or `m`
- **THEN** the selected hunk SHALL be resolved with the chosen side and the result pane SHALL update in place

#### Scenario: Whole-side apply
- **WHEN** the user presses `I`, `B`, `C`, or `M`
- **THEN** every hunk in the merge SHALL be resolved with the chosen side, and the auto-choice restore action `a` SHALL reset to the initial auto picks

### Requirement: Compare And Merge State Preservation Across File Lifecycle

Compare and merge tabs SHALL stay attached to the correct commit-side path when the working-tree file is renamed, retarget the working-tree side on rename, and cleanly close when the working-tree file is deleted. Reopening the same compare or merge target SHALL reuse the existing tab instead of opening duplicates.

#### Scenario: File renamed while compare tab is open
- **WHEN** a compare tab is open for `path/a.txt` against commit `X`, and the user renames the working-tree file to `path/b.txt`
- **THEN** the compare tab SHALL continue to compare against commit `X`'s `path/a.txt` side, while the live side SHALL point at `path/b.txt`

#### Scenario: Reopening an existing compare
- **WHEN** the user triggers compare for a path and commit that already has an open compare tab
- **THEN** MicroIDE SHALL activate the existing tab rather than opening a duplicate

### Requirement: Diff And Merge Are Measured

Diff and merge code paths SHALL be covered by the `microide_diff_bench` utility and SHALL have published warm and cold timing expectations. Changes that modify the diff, compare, or merge hot paths SHALL include before-and-after benchmark output in the change record.

#### Scenario: Change touches diff hot path
- **WHEN** a change modifies compare hunk construction, merge hunk construction, shared row-decoration build, or the compare/merge paint path
- **THEN** the change record SHALL include `microide_diff_bench` before-and-after output demonstrating no regression beyond the documented variance, or SHALL call out and justify a knowing regression

### Requirement: Low-Contrast Diff Decorations Preserve Text Legibility

Compare and merge surfaces SHALL use low-contrast fill colors for added, removed, and conflicted rows. Foreground text color SHALL remain neutral and SHALL NOT inherit a red, green, or orange tint from the row decoration.

#### Scenario: Added row keeps neutral text
- **WHEN** a compare or merge row is rendered with an added-line decoration
- **THEN** the row fill SHALL remain visually distinguishable while the text itself stays neutral and readable

#### Scenario: Conflict row stays distinguishable without overpowering the text
- **WHEN** a merge conflict row is rendered
- **THEN** the conflict state SHALL remain visible through the low-contrast palette without overwhelming the foreground glyphs

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

