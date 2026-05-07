# editor-multicursor-and-wrap Specification

## Purpose
TBD - created by archiving change editor-project-scope-polish. Update Purpose after archive.
## Requirements
### Requirement: Multiple Carets Operate As One Editor Command

MicroIDE SHALL allow one primary selection/caret and zero or more secondary selections/carets in a single editor viewport. Insertion, delete/backspace, paste, indent/outdent, and line-wise edit commands SHALL apply to every active selection in one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically.

#### Scenario: Typing with two carets
- **WHEN** the user places two carets on separate lines and types `//`
- **THEN** the editor SHALL insert `//` at both carets and record one undo step

#### Scenario: Undo restores the pre-command caret set
- **WHEN** the user performs a multi-caret paste and then triggers undo
- **THEN** every insertion from that multi-caret command SHALL be removed together and the pre-command caret set SHALL be restored

### Requirement: Soft Wrap Is A Project-Scoped Editor View Mode

MicroIDE SHALL support soft wrap as a project-scoped editor presentation mode that wraps long logical lines to the visible viewport width without modifying file contents. When soft wrap is enabled, the wrapped-row layout produced by the editor viewport SHALL be the single source of truth for: (a) editor text painting, (b) gutter line-number painting, (c) Up/Down/PageUp/PageDown caret motion for the primary caret and every secondary caret, (d) mouse hit-testing, and (e) vertical scroll position. The wrapped-row layout SHALL be cached and SHALL only be recomputed when the document layout revision, the active tab size, or the viewport's visible-column width changes; it SHALL NOT be recomputed per frame or per keystroke.

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

#### Scenario: Click on a continuation row places caret at the right logical column
- **WHEN** soft wrap is enabled and the user clicks on a continuation row of a wrapped logical line
- **THEN** the caret SHALL be placed at the logical column corresponding to that visual row's `visual_start` plus the clicked visual column offset, not at the start of the logical line

#### Scenario: Wrap layout is cached across frames
- **WHEN** soft wrap is enabled, the document layout revision is unchanged, the tab size is unchanged, and the viewport visible-column width is unchanged between two consecutive frames
- **THEN** the second frame SHALL reuse the cached wrapped-row layout without recomputing wrap segments

### Requirement: Code Surfaces Render Without Character Ligatures

Editor, compare, and merge text surfaces SHALL render code without discretionary character ligatures or glyph substitution that merges multiple codepoints into one joined glyph.

#### Scenario: Ligature-forming sequence stays literal
- **WHEN** a file contains a sequence such as `!=`, `->`, or `=>`
- **THEN** each character in the sequence SHALL remain individually visible and cursor-addressable in editor-family surfaces
