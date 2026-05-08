## ADDED Requirements

### Requirement: Multi-Caret Set Is Extended Through Add-At-Match Commands

MicroIDE SHALL provide multi-caret promotion commands that grow the active caret set without leaving the multi-caret model: `Add Cursor At Next Match` SHALL select the word under the primary caret as a seed if no selection is active, then on each invocation locate the next textually-equal occurrence below the last caret (wrapping at end-of-document once before reporting "no more matches") and promote that occurrence to a secondary caret with the same selection range as the seed. `Add Cursor At All Matches In Selection` SHALL replace every textually-equal occurrence inside the active selection with a secondary caret of the same length as the seed. Both commands SHALL preserve the multi-caret invariants from the existing requirement: every caret participates in subsequent insertion, deletion, paste, indent/outdent, and line-wise commands as one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically. Newly promoted carets SHALL respect case sensitivity declared in `editor.search.case_sensitive` (default off).

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

## MODIFIED Requirements

### Requirement: Multiple Carets Operate As One Editor Command

MicroIDE SHALL allow one primary selection/caret and zero or more secondary selections/carets in a single editor viewport. Insertion, delete/backspace, paste, indent/outdent, and line-wise edit commands SHALL apply to every active selection in one logical editor command, and undo/redo SHALL revert or reapply the full multi-caret change atomically. The multi-caret set SHALL be the single source of truth for promotion commands such as `Add Cursor At Next Match` and `Add Cursor At All Matches In Selection`; promoted carets SHALL participate in every multi-caret invariant defined here without exception.

#### Scenario: Typing with two carets
- **WHEN** the user places two carets on separate lines and types `//`
- **THEN** the editor SHALL insert `//` at both carets and record one undo step

#### Scenario: Undo restores the pre-command caret set
- **WHEN** the user performs a multi-caret paste and then triggers undo
- **THEN** every insertion from that multi-caret command SHALL be removed together and the pre-command caret set SHALL be restored

#### Scenario: Promoted caret behaves like a manually-placed caret
- **WHEN** a caret was added by `Add Cursor At Next Match` and the user invokes any multi-caret-aware command (insert, backspace, indent, paste, line-wise edit)
- **THEN** the promoted caret SHALL participate in the command identically to a manually-placed caret, and undo SHALL revert the command including the promoted caret's contribution as one atomic step
