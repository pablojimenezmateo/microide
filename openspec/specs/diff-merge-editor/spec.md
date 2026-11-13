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

### Requirement: Merge Conflicts Are Classified
Three-way merge tabs SHALL classify each conflict using explicit conflict kinds including both-modified, add/add, delete/modify, rename/rename, rename/delete, file/directory, binary, submodule, mode, and line-ending or whitespace-heavy text conflicts when Git state exposes enough information.

#### Scenario: Classification uses authoritative Git sources
- **WHEN** MicroIDE classifies conflict kinds for a merge tab
- **THEN** it SHALL derive classifications from authoritative Git status and index conflict sources rather than UI labels or guessed path heuristics

#### Scenario: Delete modify conflict
- **WHEN** one side deletes a file and the other side modifies it
- **THEN** the merge resolver SHALL identify the conflict as delete/modify and SHALL require the user to choose whether the result file exists

#### Scenario: Binary conflict
- **WHEN** Git reports a binary conflict
- **THEN** the merge resolver SHALL show a binary conflict summary and SHALL NOT present text hunk controls that cannot apply

### Requirement: Merge Resolver Labels Are Unambiguous
Merge tabs SHALL display unmistakable labels for current/ours, result, incoming/theirs, and base/common ancestor when base is available. Labels SHALL include branch or ref context where known and SHALL NOT rely on color alone.

#### Scenario: Rebase semantics avoid ambiguous ours/theirs labels
- **WHEN** a conflict originates from rebase or cherry-pick context
- **THEN** the resolver SHALL prefer concrete ref or commit labels over bare "ours/theirs" wording when both are available

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

#### Scenario: A hunk inside a comment or string opened above it
- **WHEN** a hunk's first line lies inside a block comment, a multi-line string, a raw string or a here-document that opened in the unchanged text above it
- **THEN** the hunk SHALL be read from that state, as a lex of the whole file reads it, and not as code

#### Scenario: Raw strings and here-documents are literals
- **WHEN** a C++ `R"d(...)d"`, a Rust `r#"..."#` or a shell/Ruby/Perl here-document changes
- **THEN** it SHALL be read as one string literal, its delimiters and quotes inside it included

#### Scenario: Equal-cost novelty sits where a reader expects it
- **WHEN** novel nodes could sit at several positions at the same cost (an item appended to a list of equal items, a new block around an existing one)
- **THEN** the novelty SHALL move only between equal nodes, to the placement that covers whole lines, else touches fewer lines, else ends its line, else is the latest; and a new delimiter pair around an existing one SHALL be the inner pair

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
- **THEN** the rebuild SHALL reuse the refinement of every other hunk whose text and lexical starting state are unchanged

### Requirement: Moved Code Is Shown As Moved
A run of whole deleted lines in one hunk whose text, indentation aside, reappears as a run of whole added lines in another hunk SHALL be shown as moved on both sides, in a colour of its own, and not as removed and added. A block SHALL carry at least 20 alphanumeric characters to count, so that a lone brace or keyword reappearing elsewhere is not a move. Moves SHALL NOT change hunks, staging or patches.

#### Scenario: A function moved down the file
- **WHEN** a function is cut from the top of a file and pasted, re-indented or not, at the bottom
- **THEN** its old lines and its new lines SHALL be shown as moved

#### Scenario: A move with an edit
- **WHEN** the moved lines were also edited so that no run of 20 alphanumerics is unchanged
- **THEN** the lines SHALL be shown as removed and added

### Requirement: Diffs And Merges In A Split Stay In Their Pane
With the editor split into groups, a compare or merge tab SHALL lay out, paint and take pointer input inside the pane of the group it belongs to, as an editor does; every other pane SHALL keep showing its own front tab, and a compare or merge in a pane without focus SHALL still be painted there. A click into such a pane SHALL focus it; the wheel SHALL scroll the pane under the pointer without moving focus. The find widget and the debug toolbar SHALL float over the focused pane.

#### Scenario: A merge opened beside an editor
- **WHEN** a merge tab is active in one group of a split and an editor in another
- **THEN** the editor SHALL stay visible and usable in its pane, and no merge control SHALL lie outside the merge's pane
