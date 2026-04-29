## ADDED Requirements

### Requirement: Single Source-Of-Truth Single-Line Representation

The repository SHALL hold exactly one canonical single-line edit representation, `editor::SingleLineEditor`. The legacy `util::SingleLineTextState` and its free helpers (`util::Set/Get/Insert*`) SHALL be deleted; every workspace state model that currently stores `util::SingleLineTextState` SHALL be migrated to store `editor::SingleLineEditor` directly.

#### Scenario: Legacy single-line type is removed
- **WHEN** the source tree is searched
- **THEN** no file SHALL include `util/SingleLineText.h`, no symbol `util::SingleLineTextState` SHALL exist, and the files `src/util/SingleLineText.h` and `src/util/SingleLineText.cpp` SHALL be deleted

#### Scenario: Workspace state holds the canonical model
- **WHEN** a workspace state model declares a field for a single-line input (command, prompt, search, overlay, sidebar)
- **THEN** the field SHALL have type `editor::SingleLineEditor` (or an explicit wrapper composing it), and SHALL NOT use the legacy type

#### Scenario: View models reference the canonical model
- **WHEN** a render view-model struct exposes a single-line input pointer
- **THEN** the pointer SHALL have type `const editor::SingleLineEditor*`, and SHALL NOT reference the legacy type

### Requirement: Chat Composer Reuses Shared Primitives Where Behavior Is Equivalent

The chat composer SHALL continue to use a multiline surface model for genuinely multiline operations (newline insertion, vertical caret movement, page navigation), but SHALL reuse `editor::SingleLineEditor`/`SingleLineKeyHandler` primitives for behavior that is already equivalent to single-line edit (selection-range invariants, copy/cut/paste/select-all on a single visible line). The documented multiline exception SHALL shrink accordingly.

#### Scenario: Equivalent behavior is shared
- **WHEN** the chat composer handles selection-range, clipboard, or select-all events on a single visible line
- **THEN** it SHALL route through the shared primitives, and SHALL NOT duplicate the corresponding logic
