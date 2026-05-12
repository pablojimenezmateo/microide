# editor-language-pair-and-indent Specification

## Purpose

Matching-bracket highlight, jump-to-matching-bracket, auto-close pairs, surround-selection-with-pair, language-aware smart indent on Enter / close-bracket dedent, and plugin-declared bracket and pair contracts. This capability defines how the host editor consumes a `WorkspaceLanguageContract` to deliver language-aware pair handling and indent behavior while keeping plugin contributions to data only.

## Requirements

### Requirement: Plugin-Declared Language Contract Drives Pair, Comment, And Indent Behavior

The host SHALL maintain a `WorkspaceLanguageContract` service keyed by `language_id` that, for each language, MAY declare: bracket pairs, auto-close pairs, surround pairs, a line comment marker, a block comment marker pair, indent-after-open regex hints, dedent-on-close regex hints, fold markers, and snippets. The contract SHALL be assembled from three sources merged in priority order (later overriding earlier): (1) a host-shipped default table covering the languages built into the syntax registry; (2) plugin contributions through the `PluginHost` Lua tables `ctx.brackets`, `ctx.comments`, `ctx.indents`, and `ctx.snippets`; (3) user-level and project-level overrides through `WorkspaceSettingsRegistry`. The active editor tab SHALL receive a `LanguageContractView` resolved from the tab's `language_id`, and SHALL refresh the view on plugin reload, on language id change, and on contract revision change. Plugin contributions SHALL NOT mutate live render state directly; only host code SHALL read the contract.

#### Scenario: Plugin declares brackets and comment markers
- **WHEN** a plugin's `init.lua` calls `ctx.brackets.add{ language = "ts", pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"}}}` and `ctx.comments.add{ language = "ts", line = "//", block = {"/*", "*/"}}`
- **THEN** the host language contract for `ts` SHALL include those pairs and markers, and editor surfaces with a `ts` buffer SHALL consume them for matching, auto-close, surround, and toggle-comment commands

#### Scenario: User override masks plugin contribution
- **WHEN** a plugin declares `line = "//"` for `python` and the user sets `editor.comments.python.line = "#"` in user config
- **THEN** the resolved contract for `python` SHALL report `line = "#"`, and toggle-line-comment SHALL produce `#` markers

#### Scenario: Contract refresh on language change
- **WHEN** the active editor tab's `language_id` changes from `cpp` to `python`
- **THEN** the tab's `LanguageContractView` SHALL be replaced with the `python` resolution, and subsequent typed input SHALL behave according to the `python` contract

### Requirement: Editor Highlights And Jumps To Matching Brackets

When the caret is adjacent to a bracket character that the active language contract declares as part of a bracket pair, the editor SHALL paint the matching bracket of that pair with a low-contrast emphasis on both sides, computed by a balanced scan that respects the per-line `SyntaxState` so brackets inside string and comment regions SHALL NOT be matched. The editor SHALL provide a `Jump To Matching Bracket` command that, when invoked with the caret adjacent to a recognized bracket, places the caret immediately after the matching bracket on the other side of the pair.

#### Scenario: Caret adjacent to opener highlights closer
- **WHEN** the caret is immediately after `{` in `void f() { ... }` and the active language contract declares the `{`/`}` pair
- **THEN** both the `{` and the matching `}` SHALL render with the bracket-match emphasis on the next frame

#### Scenario: Caret inside string does not match
- **WHEN** the caret is adjacent to a `{` that lies inside a string literal as classified by the syntax highlighter
- **THEN** the editor SHALL NOT paint a bracket-match emphasis and `Jump To Matching Bracket` SHALL be a no-op for that position

#### Scenario: Jump to matching bracket
- **WHEN** the caret is immediately after `(` and the user invokes `editor.jumpToMatchingBracket`
- **THEN** the caret SHALL move to the position immediately after the matching `)`, and any selection extension modifier SHALL extend selection across that range

### Requirement: Auto-Close Pairs On Typed Input

When the user types an open character that the active language contract declares as the open side of an auto-close pair, the editor SHALL insert the corresponding close character immediately after the caret and leave the caret between them, with the entire transformation forming one applied edit so undo reverts the whole pair as a single step. When the user types a close character whose next-character position already contains that exact close character, the editor SHALL skip the typed character and advance the caret over the existing close instead of inserting a duplicate. Auto-close SHALL NOT trigger inside string or comment regions when the contract declares the pair's `inhibit_in_strings` or `inhibit_in_comments` flag.

#### Scenario: Type open inserts pair
- **WHEN** the contract declares `(` / `)` as an auto-close pair and the user types `(` with no selection
- **THEN** the editor SHALL replace nothing with `()` and place the caret between the two characters, and a single undo SHALL remove the entire `()` together

#### Scenario: Type close skips over existing close
- **WHEN** the buffer contains `()` with the caret between the two characters and the user types `)`
- **THEN** the editor SHALL not insert a second `)` and SHALL advance the caret to the position immediately after the existing `)`

#### Scenario: Auto-close inhibited inside string
- **WHEN** the caret is inside a string literal as classified by the syntax highlighter and the contract declares `(` / `)` with `inhibit_in_strings = true`
- **THEN** typing `(` SHALL insert only the literal `(` and SHALL NOT insert a paired `)`

### Requirement: Typed Pair Surrounds Non-Empty Selection

When the user types an open character that the active language contract declares as the open side of a surround pair while one or more selections are non-empty, the editor SHALL replace each selection with `<open><selection><close>` and SHALL preserve the selection on the inner content so subsequent operations apply to the original text. The transformation SHALL form one applied edit per command so a single undo reverts every surround at once across all carets.

#### Scenario: Surround single selection with pair
- **WHEN** the user has the word `name` selected in a buffer with no language contract loaded but defaults declare `(` / `)` as a surround pair, and the user types `(`
- **THEN** the buffer SHALL contain `(name)` in place of `name`, the inner `name` SHALL remain selected, and a single undo SHALL restore the original `name`

#### Scenario: Surround multi-caret selections
- **WHEN** two non-empty selections exist on different lines and the user types `"`
- **THEN** each selection SHALL be wrapped with `"..."` independently, and a single undo SHALL restore both selections to their pre-surround state

### Requirement: Smart Indent On Enter And Dedent On Close

On `InsertNewline`, when the active language contract declares an indent-after-open hint that matches the line text up to the caret, the editor SHALL insert the new line with one extra indent unit beyond the previous line's leading indent, computed using the active `indent_width` and `soft_tabs` settings. When the caret on Enter sits between an open and the matching close of a declared bracket pair, the editor SHALL insert two newlines and place the caret on the middle line at one extra indent step (the "split braces" idiom). When the user types a close character that the contract declares as part of a dedent-on-close hint on a line that contains only whitespace before the typed character, the editor SHALL remove one indent unit's worth of leading whitespace from that line before inserting the typed character.

#### Scenario: Enter after open brace indents one step
- **WHEN** the contract declares `{` as an indent-after-open hint, the previous line ends with `{`, and the user presses Enter at the end of that line
- **THEN** the new line SHALL contain the previous line's leading indent plus one indent unit, and the caret SHALL be at the end of that new indent

#### Scenario: Enter between matched braces splits with extra indent
- **WHEN** the buffer contains `{|}` with the caret between the matched `{` and `}` and the user presses Enter
- **THEN** the buffer SHALL contain three lines: the line with `{`, a middle line containing one extra indent unit, and a line with `}`, with the caret on the middle line at the extra indent

#### Scenario: Type close on indent-only line dedents
- **WHEN** the contract declares `}` as a dedent-on-close hint, the current line contains only the opener's indent plus one extra unit, and the user types `}`
- **THEN** the line SHALL be reduced to the opener's indent followed by `}`, and the entire transformation (dedent + close insertion) SHALL be one applied edit

### Requirement: Pair And Indent Hot Paths Stay Within Per-Path Performance Budgets

Bracket-match scan, auto-close pair insert, surround-with-pair, smart indent on Enter, and dedent-on-close SHALL each fit within their committed harness budgets and SHALL avoid per-keystroke heap allocation beyond the existing applied-edit pipeline. Bracket-match pair resolution SHALL be cached per `(viewport_pointer, layout_revision, primary_caret_line, primary_caret_column)` so consecutive frames with no caret movement do not re-scan. Auto-close, surround, smart indent, and dedent-on-close transformations SHALL produce exactly one applied edit per typed event so the existing edit pipeline amortizes their cost. Each path SHALL have a dedicated harness scenario committed under `tests/perf/baselines/`.

#### Scenario: Bracket match scan stays inside the documented budget
- **WHEN** the harness scenario `editor_bracket_match_caret_motion` runs against a deeply-nested 50000-line fixture and exercises caret motion through nested brackets
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances

#### Scenario: Auto-close on typing stays inside the typing-large-file budget
- **WHEN** the harness scenario `editor_auto_close_typing` runs against a 50000-line fixture by holding an open-pair key
- **THEN** per-keystroke auto-close SHALL fit within the committed baseline and SHALL NOT regress the aggregate `typing_large_file` baseline beyond the documented tolerance

#### Scenario: Smart indent does not regress typing latency
- **WHEN** the harness scenario `editor_smart_indent_typing` runs against a 50000-line fixture and exercises Enter-after-open-brace
- **THEN** per-Enter smart-indent SHALL fit within the committed baseline and SHALL NOT regress the aggregate `typing_large_file` baseline beyond tolerance

#### Scenario: Surround multi-caret stays inside the documented budget
- **WHEN** the harness scenario `editor_surround_multi_caret` runs with 8 carets and 80-character selections on each
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances and the entire transformation SHALL form one applied edit per command

### Requirement: Every Pair And Indent Capability Is User-Disable-Able

Every capability defined in this spec — bracket-match highlight, jump-to-matching-bracket, auto-close pairs, surround selection, and smart indent / dedent-on-close — SHALL be individually enable/disable-able by the user through three independent surfaces: (1) a `WorkspaceSettingsRegistry` boolean keyed `editor.brackets.match_highlight.enabled`, `editor.brackets.auto_close.enabled`, `editor.brackets.surround.enabled`, and `editor.indent.smart.enabled` respectively, persisted at user and project scope with project overriding user; (2) a stable `ActionId` toggle command registered in `WorkspaceCommandRegistry` and bindable through `WorkspaceKeybindingRegistry`; and (3) a checkable menu entry registered in `WorkspaceMenuRegistry` under `Preferences → Editor` for input toggles. When a capability is disabled the editor SHALL revert to the prior built-in behavior with no residual UI: typed input SHALL produce literal characters with no auto-close, surround, or smart indent, and bracket-match emphasis SHALL not paint. The Settings overlay SHALL list every toggle from this spec in a single "Editor → Essentials → Pair And Indent" group.

#### Scenario: User disables auto-close pairs from the menu
- **WHEN** the user opens `Preferences → Editor` and clicks the checkable `Auto-Close Brackets` entry while it is checked
- **THEN** `editor.brackets.auto_close.enabled` SHALL flip to `false`, typing `(` SHALL insert only the literal `(`, and the menu entry SHALL render unchecked

#### Scenario: Smart indent disabled falls back to leading-whitespace preservation
- **WHEN** `editor.indent.smart.enabled` is `false` and the user presses Enter at the end of a line ending in `{`
- **THEN** the new line SHALL contain only the previous line's leading whitespace and SHALL NOT add an extra indent unit, matching the editor's prior built-in behavior

#### Scenario: Plugin contract remains registered while disabled
- **WHEN** a plugin has contributed brackets and comments and the user disables auto-close pairs
- **THEN** the language contract SHALL still report the plugin's bracket pairs (so jump-to-matching-bracket and toggle-comment continue to function based on the same data), and re-enabling auto-close SHALL pick the data back up without a plugin reload
