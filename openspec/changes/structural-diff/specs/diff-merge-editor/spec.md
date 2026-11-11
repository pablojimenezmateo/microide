## ADDED Requirements

### Requirement: Compare Hunks Are Refined Structurally
Compare tabs SHALL refine every hunk structurally, in the manner of difftastic: each side is read as a syntax forest of atoms (identifiers, numbers, operators, whole strings, whole comments) and delimited lists, using the file language's comment and bracket rules, the two forests are matched as a shortest path in which equal subtrees are cheap and one-sided nodes expensive, and only the tokens left without a match SHALL be shown as changed (novel).

#### Scenario: Code wrapped in a new block
- **WHEN** a compare shows lines that were indented into a new `if (…) { … }` block
- **THEN** only the new block's head and its closing brace SHALL be marked novel, and the re-indented body lines SHALL be shown paired with their old lines as a layout-only change

#### Scenario: A call re-wrapped across lines
- **WHEN** a call written on one line is re-wrapped over several lines with no token changed
- **THEN** the hunk SHALL still be shown and stageable, and no token on either side SHALL be marked novel

#### Scenario: One argument changes
- **WHEN** one argument of a call is replaced
- **THEN** only the replaced argument SHALL be marked novel on each side

#### Scenario: A comment is edited
- **WHEN** one word of a comment changes
- **THEN** only that word SHALL be marked novel on each side

### Requirement: Structural Refinement Does Not Change Hunks
The line diff SHALL remain the authority on which lines changed: hunk membership, staging, discarding and generated patches SHALL NOT depend on the structural refinement. Within a hunk, rows SHALL pair the left and right lines that share the most matched tokens, in order on both sides.

#### Scenario: Layout-only lines stay stageable
- **WHEN** a hunk contains lines that changed only in indentation
- **THEN** those lines SHALL be part of the hunk, stageable and discardable, and SHALL carry no novel token

#### Scenario: A signature gains a parameter
- **WHEN** a function signature gains a parameter
- **THEN** the old and new signature lines SHALL share a row and only the added parameter SHALL be novel

### Requirement: Structural Refinement Is Bounded
The structural search SHALL be bounded per gap, never by file size: a gap too large to search, or whose search exceeds its vertex budget, SHALL be matched by a flat token longest-common-subsequence, which still marks exactly the tokens it cannot match. An in-place rebuild of a compare model SHALL refine only the hunks whose text changed.

#### Scenario: A large rewritten hunk
- **WHEN** a hunk is too large for the structural search
- **THEN** its tokens SHALL be matched by the flat token diff, every line SHALL still be covered by exactly one row, and the build SHALL stay bounded

#### Scenario: Typing in an editable compare
- **WHEN** the user edits one hunk of an editable compare pane
- **THEN** the rebuild SHALL reuse the refinement of every other hunk

## MODIFIED Requirements

### Requirement: Diff Semantics Do Not Degrade By File Size

Diff and merge semantics SHALL NOT silently degrade, truncate, or fall back to coarser algorithms based on file-size thresholds. Performance optimizations MAY trade CPU for memory but SHALL NOT reduce the correctness of the displayed hunks. The structural refinement of a hunk is bounded by the size of its search graph, not by the size of the file: a hunk whose search exceeds the budget falls back to the flat token diff, which changes how finely the hunk is matched but never which lines are shown as changed.

#### Scenario: Large file opened in compare
- **WHEN** a compare tab is opened for a file that exceeds any large-file heuristic threshold
- **THEN** the compare tab SHALL compute and display hunks using the same algorithm used for small files, with no "diff disabled" or "diff truncated" fallback

#### Scenario: Merge conflict in a large file
- **WHEN** a three-way merge tab is opened against a file exceeding the large-file heuristic
- **THEN** every conflict hunk SHALL be resolvable through the normal per-hunk and whole-side apply actions, without the large-file path disabling merge controls

#### Scenario: A small hunk in a huge file
- **WHEN** a compare tab is opened for a very large file with a small change
- **THEN** that change SHALL be refined structurally exactly as in a small file

### Requirement: Low-Contrast Diff Decorations Preserve Text Legibility

Compare and merge surfaces SHALL use low-contrast fill colors for added, removed, and conflicted rows. Foreground text color SHALL remain neutral and SHALL NOT inherit a red, green, or orange tint from the row decoration. In a compare tab the left side reads as removed and the right side as added: a side whose line carries novel tokens gets the low-contrast wash of its color and a stronger fill behind each novel token, and a side whose line changed only in layout gets neither, keeping a muted edge stripe.

#### Scenario: Added row keeps neutral text
- **WHEN** a compare or merge row is rendered with an added-line decoration
- **THEN** the row fill SHALL remain visually distinguishable while the text itself stays neutral and readable

#### Scenario: Conflict row stays distinguishable without overpowering the text
- **WHEN** a merge conflict row is rendered
- **THEN** the conflict state SHALL remain visible through the low-contrast palette without overwhelming the foreground glyphs

#### Scenario: Novel tokens are emphasised, layout changes are not
- **WHEN** a compare row pairs a line whose only novel token is one identifier with a re-indented line that has none
- **THEN** the identifier SHALL carry the emphasis fill on its side, that side SHALL carry the row wash, and the layout-only side SHALL carry no wash and a muted edge stripe

### Requirement: Diff Review Presentation Details
Compare tabs SHALL support structural highlighting of novel tokens inside changed lines, context expansion above and below hunks, collapsed unchanged regions with line counts, sticky hunk headers where layout permits, syntax highlighting on both sides, whitespace visualization, ignore-whitespace mode, and metadata rows for binary, rename, mode, line-ending, and submodule changes.

#### Scenario: Collapsed context is expanded
- **WHEN** the user expands context above a collapsed hunk
- **THEN** the compare tab SHALL reveal additional unchanged rows while preserving the selected hunk and corresponding file-line mapping, and the reveal SHALL remain in effect for the lifetime of the open compare tab unless the tab is closed

#### Scenario: Change-overview lane follows collapsed presentation
- **WHEN** unchanged compare context is collapsed or later expanded
- **THEN** the compare tab's change-overview lane SHALL position changed-run markers against the current visible presentation rows so collapsed gaps shrink in the lane until that context is expanded again

#### Scenario: Change-overview lane mutes layout-only runs
- **WHEN** a run of changed rows carries no novel token on either side
- **THEN** the change-overview lane SHALL draw that run in a muted color, distinct from the runs that carry novel tokens

#### Scenario: Binary file is compared
- **WHEN** the selected file is binary
- **THEN** the compare tab SHALL show a binary-file summary and SHALL NOT attempt to render invalid text hunks

#### Scenario: Rename and mode change are compared
- **WHEN** a file was renamed and its executable bit changed
- **THEN** the compare tab SHALL show both the old-to-new path and mode-change metadata in the file summary
