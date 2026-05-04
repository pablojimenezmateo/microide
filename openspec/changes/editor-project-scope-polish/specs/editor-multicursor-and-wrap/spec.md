## ADDED Requirements

### Requirement: Multiple Carets Operate As One Editor Command

MicroIDE SHALL allow one primary selection/caret and zero or more secondary selections/carets in a single editor viewport. Insertion, delete/backspace, paste, indent/outdent, and line-wise edit commands SHALL apply to every active selection in one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically.

#### Scenario: Typing with two carets
- **WHEN** the user places two carets on separate lines and types `//`
- **THEN** the editor SHALL insert `//` at both carets and record one undo step

#### Scenario: Undo restores the pre-command caret set
- **WHEN** the user performs a multi-caret paste and then triggers undo
- **THEN** every insertion from that multi-caret command SHALL be removed together and the pre-command caret set SHALL be restored

### Requirement: Soft Wrap Is A Project-Scoped Editor View Mode

MicroIDE SHALL support soft wrap as a project-scoped editor presentation mode that wraps long logical lines to the visible viewport width without modifying file contents. Wrapped layout, caret motion, vertical navigation, hit-testing, and scroll position SHALL use the same wrapped-line map.

#### Scenario: Wrapped caret motion tracks visible rows
- **WHEN** soft wrap is enabled and one logical line spans multiple visible rows
- **THEN** Up/Down navigation and mouse hit-testing SHALL target the visible wrapped rows rather than jumping by whole logical lines

#### Scenario: Wrap mode persists per project
- **WHEN** the user enables soft wrap in project A, switches to project B with wrap disabled, and returns to project A
- **THEN** project A SHALL reopen with soft wrap still enabled and project B SHALL retain its own wrap mode

### Requirement: Code Surfaces Render Without Character Ligatures

Editor, compare, and merge text surfaces SHALL render code without discretionary character ligatures or glyph substitution that merges multiple codepoints into one joined glyph.

#### Scenario: Ligature-forming sequence stays literal
- **WHEN** a file contains a sequence such as `!=`, `->`, or `=>`
- **THEN** each character in the sequence SHALL remain individually visible and cursor-addressable in editor-family surfaces
