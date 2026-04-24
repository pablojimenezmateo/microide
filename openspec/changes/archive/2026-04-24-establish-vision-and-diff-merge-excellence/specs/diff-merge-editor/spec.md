## ADDED Requirements

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
