## ADDED Requirements

### Requirement: Host-Owned Modal Settings Overlay

MicroIDE SHALL expose a host-owned modal settings overlay reachable from the menu bar (Preferences → Settings…), the status bar, and a keyboard accelerator. The overlay SHALL render as a single modal surface in the editor area using the existing prompt-overlay pattern; it SHALL NOT spawn an OS dialog and SHALL NOT replace the editor or chat workflows.

#### Scenario: Overlay opens and consumes input focus
- **WHEN** the user invokes the Settings overlay action
- **THEN** the overlay SHALL render at the centered prompt-surface rectangle, capture keyboard focus, dim the editor area, and route Escape to dismissal

#### Scenario: Overlay is host-owned
- **WHEN** the source tree is searched for settings-overlay rendering
- **THEN** the overlay rendering SHALL live in `src/workspace/WorkspaceShellRenderSettings*.cpp`, owned by `SettingsOverlayService`, and SHALL NOT be replaceable by a plugin

### Requirement: Overlay Lists All Settings With Type-Aware Editors

The overlay SHALL list every entry returned by `AllSettingInfos(plugin_host)` grouped by scope (User then Project) and source (built-in then plugin). Each entry SHALL render with a type-aware editor: toggle for `Bool`, integer stepper for `Int`, numeric stepper for `Float`, dropdown for `Enum`, and single-line text input for `String`.

#### Scenario: Built-in setting renders with the right editor
- **WHEN** a built-in setting of type `Bool` is rendered
- **THEN** the row SHALL render a toggle whose state reflects the current value, and changing the toggle SHALL persist immediately through `PersistenceService`

#### Scenario: Enum setting shows declared values only
- **WHEN** a setting of type `Enum` is rendered
- **THEN** the dropdown SHALL list exactly the declared `enum_values`, SHALL preselect the current value, and the overlay SHALL NOT accept arbitrary text input for that setting

#### Scenario: Reset to default
- **WHEN** the user activates the per-row Reset affordance
- **THEN** the overlay SHALL restore the spec's default value, persist it, and visually return the row to the un-customized style

### Requirement: Overlay Provides Search And Filter

The overlay SHALL include a single-line search input that filters the visible settings by case-insensitive substring match on `id`, `label`, and `description`.

#### Scenario: Search narrows the list
- **WHEN** the user types into the search input
- **THEN** the rendered list SHALL include only settings whose id, label, or description contains the query as a case-insensitive substring, with empty groups omitted

#### Scenario: Search clears
- **WHEN** the search input is empty
- **THEN** every grouping SHALL render in its declared order

### Requirement: Built-in Settings Catalog Covers User-Facing Gaps

`BuiltinSettingSpecs()` SHALL include the following keys in addition to the existing `editor.tab_size`, `editor.indent_width`, `editor.soft_tabs`, `editor.wrap`, `editor.colorscheme`, and `ui.scale`: `editor.font_family` (string), `editor.font_size` (int, range 8..32), `editor.line_endings` (enum: `lf`/`crlf`/`auto`), `editor.trim_trailing_whitespace` (bool), `editor.insert_final_newline` (bool), `editor.format_on_save` (bool), `editor.autosave` (enum: `off`/`on_focus_change`/`after_delay`), `editor.hover_delay_ms` (int, range 0..2000), `ui.layout_mode` (enum: `auto`/`regular`/`compact`), `ui.layout_compact_breakpoint_px` (int, range 600..2000), `ui.scrollbar_size` (enum: `compact`/`regular`/`large`), `ui.resize_handle_size` (enum: `compact`/`regular`/`large`), `ui.show_status_bar` (bool), `terminal.shell` (string), `terminal.font_size` (int, range 8..32), `diagnostics.min_severity` (enum: `hint`/`info`/`warning`/`error`).

#### Scenario: New keys round-trip through the registry
- **WHEN** any of the new built-in keys is parsed and re-serialized
- **THEN** `ParseSettingValue` SHALL succeed for every documented value and `SerializeSettingValue` SHALL produce a value that `ParseSettingValue` accepts on the next read

#### Scenario: Hover-delay setting feeds the editor hover popup
- **WHEN** `editor.hover_delay_ms` changes
- **THEN** the editor hover popup SHALL apply the new delay on the next hover, and SHALL NOT require a restart

### Requirement: AI Provider Picker Replaces Click-To-Cycle

The chat sidebar's provider rail SHALL surface an "AI provider…" affordance that opens a host-owned provider-picker sub-overlay listing every registered runtime via the same overlay infrastructure as Settings. Click-to-cycle SHALL remain available as a keyboard accelerator only (`Ctrl+Shift+P` cycle next/previous). The picker SHALL be reachable from the status-bar AI segment.

#### Scenario: Picker lists every registered provider
- **WHEN** the picker opens
- **THEN** the list SHALL contain every provider in `AiProviderRegistry`, ordered alphabetically by `display_name`, and SHALL show the current default provider with an active marker

#### Scenario: Picker offers model selection
- **WHEN** the user selects a provider
- **THEN** the picker SHALL render a model dropdown populated from the runtime's `Models()` snapshot or the provider's declared `model_options`, defaulting to the runtime-reported default model when present

#### Scenario: Picker accepts an API key when required
- **WHEN** the selected provider declares `requires_api_key = true` and `auth_status` is `KeyMissing` or `KeyInvalid`
- **THEN** the picker SHALL render a secret-input field, SHALL forward submitted text to `WorkspaceAuthProvider::SetSecret(provider_id, value)`, SHALL never echo the secret to the screen or persist it in plain text, and SHALL re-trigger an auth-check on submit

#### Scenario: Per-project default override
- **WHEN** the user enables "Use this provider for the current project only" in the picker
- **THEN** the selection SHALL persist as a project-scoped record and SHALL override the user-scope default while that project is active

### Requirement: Overlay Render Path Is Allocation-Bounded

The overlay's render path SHALL build its view model on open and on user input, and SHALL NOT allocate during steady-state idle frames. Re-renders triggered solely by mouse-move within the overlay SHALL reuse the existing view model.

#### Scenario: Idle re-render does not allocate
- **WHEN** the overlay is open, no input arrives, and the cursor moves only within the overlay rectangle
- **THEN** the per-frame render SHALL reuse the cached view model and SHALL NOT allocate new strings or vectors
