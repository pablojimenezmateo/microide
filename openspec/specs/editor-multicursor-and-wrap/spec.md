# editor-multicursor-and-wrap Specification

## Purpose
Define editor behavior for multi-caret editing, add-at-match caret promotion, folded-region motion,
and project-scoped soft wrap. These features must share one selection/caret model, produce atomic
undoable edits, and use wrapped or folded visual rows consistently for rendering, hit-testing, and
navigation.

## Requirements
### Requirement: Multiple Carets Operate As One Editor Command

MicroIDE SHALL allow one primary selection/caret and zero or more secondary selections/carets in a single editor viewport. Insertion, delete/backspace, paste, indent/outdent, and line-wise edit commands SHALL apply to every active selection in one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically. The multi-caret set SHALL be the single source of truth for promotion commands such as `Add Cursor At Next Match` and `Add Cursor At All Matches In Selection`; promoted carets SHALL participate in every multi-caret invariant defined here without exception.

#### Scenario: Typing with two carets
- **WHEN** the user places two carets on separate lines and types `//`
- **THEN** the editor SHALL insert `//` at both carets and record one undo step

#### Scenario: Undo restores the pre-command caret set
- **WHEN** the user performs a multi-caret paste and then triggers undo
- **THEN** every insertion from that multi-caret command SHALL be removed together and the pre-command caret set SHALL be restored

#### Scenario: Shift+Alt+click adds column carets on a vertical line
- **WHEN** the primary caret is on line 0 column 0 and the user Shift+Alt+clicks line 3 column 0 in the editor
- **THEN** the editor SHALL place zero-width carets on lines 0 through 3 at column 0, with the primary caret on the clicked line and secondary carets on the other lines in the range

#### Scenario: Shift+Alt off-column makes a rectangular box selection
- **WHEN** the primary caret is on line 0 column 0 and the user Shift+Alt+clicks (or drags to) a different column on another line
- **THEN** the editor SHALL make a rectangular selection: every line in the row span SHALL carry a per-line selection between the anchor column and the clicked/dragged column, with the clicked line holding the primary selection and the other lines becoming ranged secondary carets; a line shorter than both box columns SHALL collapse to a zero-width caret at end-of-line

#### Scenario: Shift+Alt+drag continuously updates the box selection
- **WHEN** the user presses Shift+Alt inside the editor and drags the pointer across lines and columns
- **THEN** the editor SHALL rebuild the rectangular selection from the fixed press anchor to the live pointer on each motion, and mouse-up SHALL end the box gesture leaving the resulting multi-caret selection in place

#### Scenario: Promoted caret behaves like a manually-placed caret
- **WHEN** a caret was added by `Add Cursor At Next Match` and the user invokes any multi-caret-aware command (insert, backspace, indent, paste, line-wise edit)
- **THEN** the promoted caret SHALL participate in the command identically to a manually-placed caret, and undo SHALL revert the command including the promoted caret's contribution as one atomic step

#### Scenario: Multi-caret surround preserves multi-line selections
- **WHEN** the primary caret and one or more secondary carets each have a non-empty selection range spanning multiple lines and the user types a configured surround opener
- **THEN** every selected range SHALL be wrapped with the matching open/close pair, each caret SHALL retain its inner selection, and undo SHALL restore the pre-command buffer and caret set atomically

### Requirement: Multi-Caret Set Is Extended Through Add-At-Match Commands

MicroIDE SHALL provide multi-caret promotion commands that grow the active caret set without leaving the multi-caret model: `Add Cursor At Next Match` SHALL select the word under the primary caret as a seed if no selection is active, then on each invocation locate the next textually-equal occurrence below the last caret (wrapping at end-of-document once before reporting "no more matches") and promote that occurrence to a secondary caret with the same selection range as the seed. `Add Cursor At All Matches In Selection` SHALL replace every textually-equal occurrence inside the active selection with a secondary caret of the same length as the seed. Both commands SHALL preserve the multi-caret invariants from the existing requirement: every caret participates in subsequent insertion, deletion, paste, indent/outdent, and line-wise commands as one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically. Newly promoted carets SHALL respect case sensitivity declared in `editor.search.case_sensitive` (default off). Invoke paths SHALL honor `editor.multicursor.add_at_match.enabled` (default on); when it is off, add-at-match actions SHALL not mutate the caret set even if dispatched.

#### Scenario: Add cursor at next match seeds from word
- **WHEN** the caret is inside `foo` with no active selection and the user invokes `editor.addCursorAtNextMatch`
- **THEN** the primary caret SHALL select `foo`, and a subsequent invocation SHALL locate the next `foo` occurrence and promote it to a secondary caret with `foo` selected and the same case-sensitivity rule used by the seed match

#### Scenario: Add cursor at all matches replaces every occurrence in selection
- **WHEN** the user has a selection covering text containing three occurrences of `name`, the primary caret has `name` selected, and the user invokes `editor.addCursorAtAllMatchesInSelection`
- **THEN** every `name` occurrence inside the selection SHALL become a caret with `name` selected, and the user's prior selection SHALL be replaced by the resulting caret set

#### Scenario: Promoted caret participates in atomic multi-caret edit
- **WHEN** two carets exist (one promoted via `editor.addCursorAtNextMatch`) and the user types `XYZ`
- **THEN** `XYZ` SHALL be inserted at both caret positions in one applied edit, and a single undo SHALL revert both insertions and SHALL restore the pre-promotion caret set

### Requirement: Folded Regions Behave As One Visible Row For Multi-Caret Vertical Motion

When code folding is active and one or more fold ranges are collapsed, every caret in the active multi-caret set SHALL traverse a collapsed fold as if it were a single visible row during Up/Down and PageUp/PageDown motion. Each caret's preferred-column anchor SHALL be preserved through the collapsed fold so that, on entering an expanded region below or above the fold, the caret returns to its preferred column when the row is wide enough.

#### Scenario: Up/Down across collapsed fold treats fold as one row
- **WHEN** a fold range from line 10 through line 20 is collapsed, a primary caret is on line 9 column 3, and a secondary caret is on line 9 column 12, and the user presses Down
- **THEN** the primary caret SHALL move to line 21 column 3 (or end of line 21 if shorter) and the secondary caret SHALL move to line 21 column 12 (or end of line 21 if shorter), each preserving its own preferred column

#### Scenario: PageDown crossing a collapsed fold counts the fold as one row
- **WHEN** PageDown advances each caret by N visible rows and a collapsed fold spans 11 logical lines inside that page
- **THEN** the page step SHALL count the collapsed fold as one row toward N, and every caret SHALL advance accordingly

### Requirement: Soft Wrap Is A Project-Scoped Editor View Mode

MicroIDE SHALL support soft wrap as a project-scoped editor presentation mode that wraps long logical lines to the visible viewport width without modifying file contents. When soft wrap is enabled, the wrapped-row layout produced by the editor viewport SHALL be the single source of truth for: (a) editor text painting, (b) gutter line-number painting, (c) Up/Down/PageUp/PageDown caret motion for the primary caret and every secondary caret, (d) mouse hit-testing, and (e) vertical scroll position. The wrapped-row layout SHALL be cached and SHALL only be recomputed when the document layout revision, the active tab size, or the viewport's visible-column width changes; it SHALL NOT be recomputed per frame or per keystroke.

Wrap break selection SHALL prefer the most recent ASCII whitespace (space or tab) boundary that lies inside the current row's window; when no such boundary exists inside the window (a single token longer than the wrap width), the layout SHALL hard-break at the column boundary so the unbreakable token still wraps.

Wrapped rows SHALL be contiguous in visual columns, so the column at which one row ends and the next begins denotes ONE text position. Vertical caret motion and mouse hit-testing that land on such a wrap boundary SHALL resolve it to the row whose END it is — the caret SHALL render at that row's trailing edge — while every other caret placement SHALL resolve it to the row it STARTS. Without this rule the caret cannot leave a wrapped row upward at all: a preferred column past a shorter row's width clamps to exactly that boundary, which then resolves back to the row the motion started from.

The preferred column that vertical motion carries SHALL be measured in ON-SCREEN cells of the caret's visual row, including any hanging indent the row is rendered with, so motion between a line's first row and its indented continuation rows keeps the caret under the same screen column.

A change to the wrap width, and a toggle of soft wrap itself, SHALL re-anchor the vertical scroll position on the logical line that was at the top of the view, and SHALL leave the scroll position within the document. Both renumber every visual row, so a scroll offset carried across one unchanged denotes a different place in the document, or none at all.

When soft wrap is enabled, horizontal scrolling SHALL be suppressed: the horizontal scroll offset SHALL clamp to zero and the editor SHALL NOT render a horizontal scrollbar.

Toggling soft wrap from any user-invoked path (View menu action, keybinding, command palette, settings overlay) SHALL invalidate the active editor surface so previously-painted rows do not persist beyond the toggle.

#### Scenario: Wrapped caret motion tracks visible rows
- **WHEN** soft wrap is enabled and one logical line spans multiple visible rows
- **THEN** Up/Down navigation and mouse hit-testing SHALL target the visible wrapped rows rather than jumping by whole logical lines

#### Scenario: Wrap mode persists per project
- **WHEN** the user enables soft wrap in project A, switches to project B with wrap disabled, and returns to project A
- **THEN** project A SHALL reopen with soft wrap still enabled and project B SHALL retain its own wrap mode

#### Scenario: Render iterates wrapped rows
- **WHEN** soft wrap is enabled and a logical line is wider than the viewport
- **THEN** the editor render path SHALL paint each wrapped segment on its own visible row, with no horizontal clipping of the logical line, and SHALL NOT paint the logical line on a single visible row

#### Scenario: Gutter shows line number only on first wrapped row
- **WHEN** soft wrap is enabled and a logical line wraps into N visible rows
- **THEN** the gutter SHALL render the logical line number on the first visible row only, and the remaining N-1 continuation rows SHALL render an empty gutter cell of the same width

#### Scenario: Multi-caret Up/Down moves every caret along visible rows
- **WHEN** soft wrap is enabled, the editor has a primary caret and one or more secondary carets, and the user presses Up or Down
- **THEN** every caret SHALL advance by exactly one visible row using its own preferred-column anchor, so all carets remain at consistent visual columns

#### Scenario: Preferred column survives wrapped continuations
- **WHEN** soft wrap is enabled and a caret moves vertically through a wrapped logical line whose continuation row is shorter than the preferred column
- **THEN** the caret SHALL clamp to the end of the continuation row for that step, and on the next vertical move into a row that is wide enough, the caret SHALL return to the preferred column

#### Scenario: Up off a wrapped row lands on the row above it
- **WHEN** soft wrap is enabled, a logical line wraps into rows whose middle row is narrower than the caret's preferred column, and the caret is on a row below it
- **THEN** each Up SHALL advance the caret exactly one visible row upward, clamping to the narrow row's end for that step, and SHALL NOT leave the caret on the row it started from

#### Scenario: Down onto a narrow wrapped row stops on that row
- **WHEN** soft wrap is enabled, the caret is on a wrapped row with a preferred column wider than the next row
- **THEN** Down SHALL place the caret at the end of that next row and SHALL NOT skip past it to the row below

#### Scenario: Vertical motion across a hanging indent keeps the screen column
- **WHEN** soft wrap is enabled, a wrapped line begins with leading whitespace so its continuation rows carry a hanging indent, and the caret moves down from the line's first row
- **THEN** the caret SHALL land at the same on-screen column, i.e. the indent cells plus the remaining offset into the continuation row's text

#### Scenario: Click past a wrapped row's last glyph stays on that row
- **WHEN** soft wrap is enabled and the user clicks to the right of the last glyph of a continuation row
- **THEN** the caret SHALL be placed at that row's end and SHALL render there, not at the start of the row below

#### Scenario: Changing the wrap width keeps the top of the view
- **WHEN** soft wrap is enabled, the view is scrolled so logical line L is at the top, and the editor's visible-column width changes
- **THEN** the view SHALL be re-anchored so logical line L is at the top again, and the scroll position SHALL remain within the re-wrapped document

#### Scenario: Click on a continuation row places caret at the right logical column
- **WHEN** soft wrap is enabled and the user clicks on a continuation row of a wrapped logical line
- **THEN** the caret SHALL be placed at the logical column corresponding to that visual row's `visual_start` plus the clicked visual column offset, not at the start of the logical line

#### Scenario: Wrap layout is cached across frames
- **WHEN** soft wrap is enabled, the document layout revision is unchanged, the tab size is unchanged, and the viewport visible-column width is unchanged between two consecutive frames
- **THEN** the second frame SHALL reuse the cached wrapped-row layout without recomputing wrap segments

#### Scenario: Wrap breaks at whitespace boundaries when one is available
- **WHEN** soft wrap is enabled and a long line contains whitespace between words inside the current row's window
- **THEN** the wrap layout SHALL end the row at the most recent whitespace and start the next row at the following non-whitespace character, so words are not split mid-token

#### Scenario: Wrap hard-breaks inside an unbreakable long token
- **WHEN** soft wrap is enabled and a single token without internal whitespace is wider than the wrap window
- **THEN** the wrap layout SHALL hard-break the token at the column boundary so wrapping still occurs

#### Scenario: Horizontal scrollbar is hidden under soft wrap
- **WHEN** soft wrap is enabled in the active editor
- **THEN** the editor surface SHALL NOT render a horizontal scrollbar and the horizontal scroll offset SHALL be zero

#### Scenario: Toggling wrap refreshes the editor surface
- **WHEN** the user toggles soft wrap from any user-invoked path while editor rows are visible
- **THEN** the editor surface SHALL be redrawn so no rows from the previous wrap mode remain on screen

### Requirement: Code Surfaces Render Without Character Ligatures

Editor, compare, and merge text surfaces SHALL render code without discretionary character ligatures or glyph substitution that merges multiple codepoints into one joined glyph.

#### Scenario: Ligature-forming sequence stays literal
- **WHEN** a file contains a sequence such as `!=`, `->`, or `=>`
- **THEN** each character in the sequence SHALL remain individually visible and cursor-addressable in editor-family surfaces
