## ADDED Requirements

### Requirement: Single Shared Single-Line Editor Model

The application SHALL provide one shared single-line editor model that owns buffer text, caret position, and an optional selection range, plus shared key handling for standard edit operations. All single-line input surfaces SHALL consume this model.

#### Scenario: Surfaces share the model
- **WHEN** any of the prompt input, command input, overlay query field, or sidebar search field handles a key event for insertion, deletion, caret movement, selection, copy, cut, paste, or select-all
- **THEN** the surface SHALL route the event through the shared `SingleLineEditor` model and `SingleLineKeyHandler`, and SHALL NOT contain its own implementation of those operations

#### Scenario: Multiline chat composer remains a separate surface model
- **WHEN** chat composer input handles multiline editing operations such as newline insertion, vertical caret movement, or page navigation
- **THEN** it MAY continue using a multiline surface model in this change, while reusing shared primitives where behavior is equivalent

#### Scenario: Surface-specific actions remain on the surface
- **WHEN** a surface needs to handle submit, history, escape semantics, or completion
- **THEN** that behavior SHALL stay in the surface and SHALL operate on the shared model's typed accessors rather than mutating its internals directly

### Requirement: Standard Edit Operations Are Tested Once

The shared single-line editor model SHALL be covered by a single regression suite that exercises every standard operation, and surface-level tests SHALL NOT need to retest those operations per surface.

#### Scenario: Editor model regression suite
- **WHEN** the test suite runs
- **THEN** a focused fixture under `microide_tests` SHALL exercise insert, backspace, delete-forward, move-left, move-right, move-home, move-end, select-all, copy, cut, paste, and selection-range invariants on the shared model

#### Scenario: Surface tests rely on the shared coverage
- **WHEN** a new single-line surface is added
- **THEN** it SHALL gain only surface-specific tests (submit, focus, dismiss, completion) and SHALL inherit standard editing correctness from the shared model coverage

### Requirement: View Models For Render Surfaces

Render surfaces SHALL consume typed view-model structs produced by a host-owned builder. View-model structs SHALL be POD-like, trivially copyable, and SHALL contain exactly the fields the surface needs.

#### Scenario: Surface declares its view model
- **WHEN** a render surface is added or modified
- **THEN** it SHALL declare a dedicated `<Surface>ViewModel` struct, the builder SHALL populate that struct from service queries, and the surface SHALL accept it as a `const&` parameter

#### Scenario: No back-references in view models
- **WHEN** a view model is constructed
- **THEN** it SHALL NOT contain pointers or references to `WorkspaceShell`, coordinators, or services, and the architectural-lint test SHALL reject such fields
