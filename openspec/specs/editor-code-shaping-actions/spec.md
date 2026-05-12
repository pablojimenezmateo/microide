# editor-code-shaping-actions Specification

## Purpose

Toggle line/block comment, move/duplicate/delete line, indent/outdent selection, sort lines, add-cursor-at-next-match, add-cursor-at-all-matches-in-selection, highlight occurrences, snippet expansion with placeholder navigation, and save-time normalization (trim trailing whitespace, ensure final newline, auto-detect indent on open). This capability defines the host-owned shaping actions and save-time transforms that operate on the active multi-caret set and on the serialized buffer at save.

## Requirements

### Requirement: Toggle Line And Block Comment Operate On The Active Selection

The editor SHALL provide `Toggle Line Comment` and `Toggle Block Comment` actions that consume the active language contract's `line` and `block` comment markers respectively. Toggle Line Comment SHALL operate on every line touched by every active selection (or the line containing the caret when the selection is empty); if every touched line already starts with the line-comment marker (after leading whitespace), the marker SHALL be removed; otherwise the marker SHALL be inserted at the minimum-leading-whitespace column shared by all touched lines. Toggle Block Comment SHALL wrap each non-empty selection with the block markers, or unwrap when the selection is already exactly wrapped. Both actions SHALL produce one applied edit per invocation so a single undo reverts the whole change including across multi-caret selections.

#### Scenario: Toggle line comment on a multi-line selection adds markers
- **WHEN** the active language contract declares `//` as the line comment marker, two lines `foo` and `  bar` are selected, and the user invokes `editor.toggleLineComment`
- **THEN** the selection SHALL become `// foo` and `//   bar`, the marker SHALL be inserted at the minimum-leading-whitespace column of the touched lines, and a single undo SHALL revert both lines together

#### Scenario: Toggle line comment removes existing markers
- **WHEN** every line in the selection already starts with the active language's line comment marker (after leading whitespace) and the user invokes `editor.toggleLineComment`
- **THEN** the marker (and any single space immediately following it) SHALL be removed from every selected line in one applied edit

#### Scenario: Toggle block comment wraps and unwraps
- **WHEN** the active language declares `/* */` block markers, the selection is `name`, and the user invokes `editor.toggleBlockComment`
- **THEN** the selection SHALL be replaced by `/*name*/`; invoking the action again SHALL restore `name`

#### Scenario: Toggle line comment with no language contract is unavailable
- **WHEN** the active editor tab has no resolved language contract or the contract declares no line comment marker
- **THEN** `editor.toggleLineComment` SHALL be reported as not enabled by the action coordinator, and invoking it SHALL be a no-op

### Requirement: Move, Duplicate, And Delete Line Operations

The editor SHALL provide line-shaping actions that operate on every active selection (line of caret when selection is empty) as one applied edit per command:

- `Move Line Up` and `Move Line Down`: swap the lines touched by the selection with the line immediately above or below, preserving the selection on the moved content.
- `Duplicate Selection`: insert a copy of the selected text immediately after the selection (or, when selection is empty, duplicate the current line below itself).
- `Delete Line`: remove every line touched by the selection (or the current line when selection is empty) and place the caret at the start of the line that follows.

All four actions SHALL participate in the multi-caret loop so each active selection moves/duplicates/deletes independently, and SHALL produce a single applied edit per invocation so undo reverts the whole change.

#### Scenario: Move line down with multi-line selection
- **WHEN** lines 5 through 7 are selected and the user invokes `editor.moveLineDown`
- **THEN** the buffer contents at lines 5 through 7 SHALL move to lines 6 through 8, the previous line 8 SHALL move to line 5, and the selection SHALL still cover the moved content (now lines 6 through 8)

#### Scenario: Duplicate selection inserts copy immediately after
- **WHEN** the user has `foo bar` selected and invokes `editor.duplicateSelection`
- **THEN** the buffer SHALL contain `foo barfoo bar` with the second `foo bar` selected and a single undo SHALL remove the duplicate

#### Scenario: Delete line with empty selection removes current line
- **WHEN** the caret is on a line and selection is empty, and the user invokes `editor.deleteLine`
- **THEN** the current line SHALL be removed entirely (including its trailing newline) and the caret SHALL land at the start of the line that took its place

#### Scenario: Multi-caret move preserves caret independence
- **WHEN** two carets exist on lines 3 and 7 and the user invokes `editor.moveLineUp`
- **THEN** line 3 SHALL swap with line 2 and line 7 SHALL swap with line 6, and a single undo SHALL revert both swaps together

### Requirement: Indent And Outdent Selection

The editor SHALL re-define `Tab` and `Shift+Tab` when one or more active selections are multi-line: `Tab` SHALL insert one indent unit at the start of every touched line; `Shift+Tab` SHALL remove up to one indent unit's worth of leading whitespace from every touched line. The transformation SHALL respect the active `soft_tabs` and `indent_width` settings and SHALL form one applied edit per invocation. When the active selection is single-line and contains text, `Tab` SHALL retain its existing tab-character / soft-tab insertion semantics rather than indenting.

#### Scenario: Multi-line Tab indents every selected line
- **WHEN** lines 4 through 6 are selected, `soft_tabs = true`, `indent_width = 4`, and the user presses Tab
- **THEN** four spaces SHALL be inserted at the start of each of lines 4, 5, and 6, and the selection SHALL still cover the same logical lines

#### Scenario: Single-line Tab inside text inserts a tab unit
- **WHEN** the selection is empty and the caret is in the middle of a line containing text, and the user presses Tab
- **THEN** the editor SHALL insert one tab unit at the caret per the existing tab-character / soft-tab insertion semantics, and SHALL NOT re-indent the line

#### Scenario: Multi-line Shift+Tab outdents only existing indent
- **WHEN** lines 4 through 6 are selected with line 5 containing only two spaces of leading whitespace and `indent_width = 4`, and the user presses Shift+Tab
- **THEN** lines 4 and 6 SHALL each lose four spaces of leading whitespace if they had at least four, line 5 SHALL lose its two spaces, and no line SHALL be modified beyond its leading whitespace

### Requirement: Sort Lines Action

The editor SHALL provide `Sort Lines Ascending` and `Sort Lines Descending` actions that, when at least one line is touched by an active selection, replace the touched line range with the same lines sorted lexicographically (case-insensitive, locale-independent). The action SHALL form one applied edit per invocation and SHALL NOT modify any line outside the touched range.

#### Scenario: Sort selected lines ascending
- **WHEN** lines 3 through 5 contain `banana`, `apple`, `cherry` and are selected, and the user invokes `editor.sortLinesAscending`
- **THEN** lines 3 through 5 SHALL contain `apple`, `banana`, `cherry`

#### Scenario: Sort with empty selection is unavailable
- **WHEN** no selection is active
- **THEN** the sort actions SHALL be reported as not enabled by the action coordinator

### Requirement: Add Cursor At Match Promotes Multi-Caret Discoverability

The editor SHALL provide `Add Cursor At Next Match` and `Add Cursor At All Matches In Selection` actions. The "next match" action SHALL: when no selection is active, select the word under the primary caret as the match seed and place the seed selection on the primary caret; on each subsequent invocation, find the next textually-equal occurrence of the seed below the last caret (wrapping at end-of-document once before reporting "no more matches"), promote that occurrence to a secondary caret with the same selection range as the seed, and SHALL respect case sensitivity declared in `editor.search.case_sensitive` (default off). The "all matches in selection" action SHALL replace every textually-equal occurrence inside the active selection with a secondary caret of the same length as the seed.

#### Scenario: Add cursor at next match seeds from word
- **WHEN** the caret is inside `foo` and no selection is active, and the user invokes `editor.addCursorAtNextMatch`
- **THEN** the primary caret SHALL select `foo`, and a subsequent invocation SHALL find the next `foo` occurrence and promote it to a secondary caret with `foo` selected

#### Scenario: Add cursor at all matches replaces every occurrence in selection
- **WHEN** the user has a selection covering text containing three occurrences of `name`, primary caret has `name` selected, and the user invokes `editor.addCursorAtAllMatchesInSelection`
- **THEN** every `name` occurrence inside the selection SHALL become a caret with `name` selected, replacing the previous selection

#### Scenario: No more matches reports without modifying carets
- **WHEN** the user invokes `editor.addCursorAtNextMatch` after the seed has wrapped past every occurrence
- **THEN** the existing caret set SHALL be unchanged, and the action coordinator SHALL emit a "no more matches" status indication

### Requirement: Highlight Occurrences Of Word Under Caret

When the primary caret is inside or adjacent to a word, the editor SHALL paint a low-contrast underlay on every textually-equal occurrence of that word in the visible viewport, computed by `RenderViewModelBuilder` and consumed by the editor render translation unit as one ordered layer of the decorated text-grid pipeline. The scan SHALL be viewport-bounded so very large files do not pay full-document cost. The underlay SHALL clear when the primary caret has no word under it or moves to a different word.

#### Scenario: Word under caret highlights matching occurrences
- **WHEN** the caret is inside the word `name` and three occurrences of `name` are visible in the viewport
- **THEN** each of the four (caret-owned plus three) `name` ranges SHALL render with the occurrence-highlight underlay

#### Scenario: Underlay clears when caret leaves word
- **WHEN** the caret moves into whitespace or punctuation between two words
- **THEN** the next frame SHALL render with no occurrence underlay

#### Scenario: Occurrence scan is viewport-bounded
- **WHEN** the editor opens a 50000-line file and only the first viewport's worth of lines is visible
- **THEN** the occurrence scan SHALL inspect at most the visible row range plus a bounded look-ahead, and SHALL NOT walk every line of the document on the first frame

### Requirement: Snippet Expansion With Placeholder Navigation

The editor SHALL provide a host-owned snippet engine that expands a snippet body in place at the active caret(s), supporting placeholder syntax of the form `${N}`, `${N:default}`, `${N:|choice1,choice2|}`, and `$0` for the final tab stop. On expansion, the engine SHALL replace the trigger range with the snippet body, position the primary caret at placeholder `1` (or all carets at every `1` placeholder when more than one exists), and register an active `SnippetSession` on the editor tab. While a session is active, `Tab` SHALL advance to the next placeholder and `Shift+Tab` SHALL advance to the previous; navigating to `$0` SHALL exit the session. Any caret motion that leaves every active placeholder, or any explicit `Escape`, SHALL cleanly exit the session. Snippets SHALL be expandable from completion entries (when the completion item is marked as a snippet) and from a dedicated `Insert Snippet…` overlay populated from the language contract's snippet list. The whole expansion plus placeholder navigation SHALL form one undo unit so a single undo unwinds the entire snippet.

#### Scenario: Snippet expansion seeds placeholder navigation
- **WHEN** the user accepts a snippet `for (${1:i} = 0; ${1} < ${2:limit}; ${1}++) { $0 }` from the completion overlay
- **THEN** the body SHALL be inserted at the caret with the three `${1}` ranges occupied by `i` and selected on every caret bound to placeholder 1, the next `Tab` SHALL move to placeholder 2 with `limit` selected, and a final `Tab` SHALL place the caret at the `$0` position and exit the session

#### Scenario: Edit during placeholder updates linked occurrences
- **WHEN** placeholder 1 is active with three linked occurrences and the user types `index`
- **THEN** every linked occurrence SHALL update to `index` synchronously as one applied edit, and a single undo SHALL revert the whole expansion

#### Scenario: Caret outside placeholder cancels session
- **WHEN** a snippet session is active with caret on placeholder 1 and the user clicks somewhere outside every active placeholder range
- **THEN** the session SHALL exit cleanly, subsequent `Tab` keys SHALL revert to their default insertion behavior, and the buffer SHALL retain whatever the user typed inside placeholders prior to leaving

### Requirement: Save-Time Normalization And Auto-Detect Indent

The editor save pipeline SHALL run, before any LSP / formatter save participants, a normalization step that, when their respective settings are enabled: (1) trims trailing whitespace on every line, (2) ensures the buffer ends with exactly one trailing newline, and (3) leaves indentation untouched. On `OpenFile`, when `editor.indent.detect_on_open` is enabled, the editor SHALL scan up to the first 256 non-blank lines, choose `soft_tabs` (true if the majority of leading-whitespace lines start with one or more spaces, false if the majority start with a tab) and `indent_width` (the GCD of leading-space counts, clamped to `[2, 8]`), and apply the result to that tab only. Detection SHALL NOT modify file contents and SHALL NOT be persisted; subsequent reopens re-detect.

#### Scenario: Save trims trailing whitespace and ensures final newline
- **WHEN** the user saves a buffer containing `foo \nbar\n\n` with both save-normalization settings enabled
- **THEN** the saved file content SHALL be `foo\nbar\n` (one trailing newline, trailing space on line 1 removed)

#### Scenario: Auto-detect picks tabs over spaces when majority indents with tabs
- **WHEN** a file has 30 leading-tab lines and 5 leading-4-space lines, `editor.indent.detect_on_open` is true
- **THEN** the tab opens with `soft_tabs = false`, `indent_width = 4` (the dominant tab visual width or the existing project default), and the project-level `editor.indent.*` setting SHALL NOT be modified

#### Scenario: Detection does not persist across reopens
- **WHEN** a tab was auto-detected with `soft_tabs = true, indent_width = 2`, the user closes the tab, manually edits the project's setting to `soft_tabs = false`, and reopens the file
- **THEN** the auto-detect run SHALL re-evaluate from file content and SHALL NOT remember the prior detection

### Requirement: Shaping And Save Hot Paths Stay Within Per-Path Performance Budgets

Add-cursor-at-next-match, occurrence scan, multi-caret line shaping, multi-line toggle-comment, sort-lines, snippet expansion, snippet placeholder linked-edit, save normalization, and auto-detect indent SHALL each fit within their committed harness budgets. Occurrence scan SHALL be viewport-bounded and cached per `(word_under_caret, layout_revision, visible_row_start, visible_row_end)`; word-under-caret detection SHALL be cached per `(layout_revision, primary_caret_line, primary_caret_column)`. Auto-detect indent SHALL inspect at most the first 256 non-blank lines so very large files do not pay file-size cost on open. Save normalization SHALL run as in-place transforms over the serialized buffer so it does not double-allocate the file content. Snippet linked-placeholder edits SHALL form one applied edit covering all linked occurrences so the existing edit pipeline amortizes the cost. Each path SHALL have a dedicated harness scenario committed under `tests/perf/baselines/`.

#### Scenario: Occurrence scan stays inside the documented budget
- **WHEN** the harness scenario `editor_occurrences_scan` runs against a 50000-line fixture in which the highlighted word appears once per line
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and the scan SHALL inspect at most the visible row range plus a bounded look-ahead

#### Scenario: Add cursor at next match stays inside the documented budget
- **WHEN** the harness scenario `editor_add_cursor_next_match` runs against a 50000-line fixture with 10000 occurrences of the seed word and the user invokes the action repeatedly
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances

#### Scenario: Multi-caret line shaping stays inside the documented budget
- **WHEN** the harness scenario `editor_shaping_multi_caret` runs with 32 carets on a 50000-line fixture and invokes `editor.moveLineDown` repeatedly
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and each invocation SHALL form exactly one applied edit

#### Scenario: Toggle-comment on a large selection stays inside the documented budget
- **WHEN** the harness scenario `editor_toggle_comment_large_selection` runs with a 1000-line selection
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and the entire transformation SHALL form one applied edit

#### Scenario: Sort lines stays inside the documented budget
- **WHEN** the harness scenario `editor_sort_lines_large` runs with a 10000-line selection
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances

#### Scenario: Snippet linked-placeholder edit stays inside the documented budget
- **WHEN** the harness scenario `editor_snippet_placeholder_edit` runs with a snippet whose placeholder has 10 linked occurrences and the user types into the placeholder
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and each typed character SHALL form exactly one applied edit covering all linked occurrences

#### Scenario: Save normalization stays inside the documented budget
- **WHEN** the harness scenario `editor_save_normalization` runs with a 1MB buffer and both `editor.save.trim_trailing_whitespace` and `editor.save.ensure_final_newline` enabled
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and SHALL NOT block input handling while running

#### Scenario: Auto-detect indent stays inside the documented budget
- **WHEN** the harness scenario `editor_indent_detect_open` runs by opening a 1MB file with `editor.indent.detect_on_open` enabled
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and the scan SHALL inspect at most the first 256 non-blank lines

### Requirement: Every Code Shaping Action Is User-Disable-Able

Every capability defined in this spec — toggle-line-comment, toggle-block-comment, move/duplicate/delete-line, indent/outdent selection, sort-lines, add-cursor-at-next-match, add-cursor-at-all-matches-in-selection, highlight-occurrences, snippet expansion, trim-trailing-whitespace-on-save, ensure-final-newline-on-save, and auto-detect-indent-on-open — SHALL be individually enable/disable-able by the user through three independent surfaces: (1) a `WorkspaceSettingsRegistry` boolean keyed under `editor.shaping.*`, `editor.multicursor.add_at_match.enabled`, `editor.occurrences.enabled`, `editor.snippets.enabled`, `editor.save.trim_trailing_whitespace`, `editor.save.ensure_final_newline`, or `editor.indent.detect_on_open` respectively, persisted at user and project scope with project overriding user; (2) a stable `ActionId` toggle command registered in `WorkspaceCommandRegistry` and bindable through `WorkspaceKeybindingRegistry`; and (3) a checkable menu entry registered in `WorkspaceMenuRegistry` under `Selection` for multi-cursor toggles, `Edit` for shaping-action toggles, or `Preferences → Editor` for save-normalization toggles. When a capability is disabled the action coordinator SHALL report its corresponding action as not enabled, executor dispatch and shaping-related editor key surfaces SHALL refuse buffer mutations where that toggle applies, the related render layer (occurrence underlay, snippet placeholder overlay) SHALL NOT paint, and save-time normalization SHALL be skipped for that step. The Settings overlay SHALL list every toggle from this spec in a single "Editor → Essentials → Shaping And Save" group.

#### Scenario: User disables occurrences highlight from the Selection menu
- **WHEN** the user opens `Selection` and clicks the checkable `Highlight Occurrences` entry while it is checked
- **THEN** `editor.occurrences.enabled` SHALL flip to `false`, the next frame SHALL paint no occurrence underlay, and the menu entry SHALL render unchecked

#### Scenario: Disabling snippet expansion still allows literal trigger insertion
- **WHEN** `editor.snippets.enabled` is `false`, a completion entry marked as a snippet is accepted
- **THEN** the completion entry's literal `insert_text` SHALL be inserted with no placeholder substitution, no snippet session SHALL be registered, and `Tab` SHALL behave as a normal indent/insertion

#### Scenario: Disabling save trim leaves trailing whitespace untouched
- **WHEN** `editor.save.trim_trailing_whitespace` is `false` and the user saves a buffer ending with `foo   \n`
- **THEN** the saved file SHALL retain the trailing spaces exactly as in the buffer

#### Scenario: Disabling auto-detect leaves project setting authoritative
- **WHEN** `editor.indent.detect_on_open` is `false` and a file is opened
- **THEN** the tab SHALL adopt `soft_tabs` and `indent_width` from the resolved user/project setting unchanged, and SHALL NOT inspect the file's leading whitespace

#### Scenario: Disabled toggle-comment shaping refuses buffer mutation
- **WHEN** `editor.shaping.toggle_comment.enabled` is `false` and `toggle-line-comment` (or block comment) is dispatched on an eligible buffer
- **THEN** every line SHALL remain byte-for-byte unchanged even if the action is invoked from the command palette

#### Scenario: Disabled line-operation shaping refuses move and multi-line Tab indent
- **WHEN** `editor.shaping.line_ops.enabled` is `false`, the user dispatches `move-line-up`, `move-line-down`, duplicate-line, delete-line, indent-lines, or outdent-lines — or presses `Tab` / `Shift+Tab` with a multi-line selection
- **THEN** the buffer SHALL remain unchanged for that input

#### Scenario: Disabled sort shaping refuses lexicographic reorder
- **WHEN** `editor.shaping.sort_lines.enabled` is `false` and sort-lines ascending or descending is dispatched on a touched line range
- **THEN** those lines SHALL retain their prior order and contents
