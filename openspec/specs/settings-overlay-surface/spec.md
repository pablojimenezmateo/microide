# settings-overlay-surface Specification

## Purpose
Define the host-owned Settings overlay surface and the settings catalog behavior behind it. The
overlay must expose built-in and plugin settings through typed editors, searchable groups, accurate
helper copy, immediate persistence, and allocation-bounded rendering.

## Requirements
### Requirement: Host-Owned Modal Settings Overlay

MicroIDE SHALL expose a host-owned modal settings overlay reachable from the menu bar (Preferences → Settings…), the status bar, and a keyboard accelerator. The overlay SHALL render as a single modal surface in the editor area using the existing prompt-overlay pattern; it SHALL NOT spawn an OS dialog and SHALL NOT replace editor workflows.

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

`BuiltinSettingSpecs()` SHALL include the following keys in addition to the existing `editor.tab_size`, `editor.indent_width`, `editor.soft_tabs`, `editor.wrap`, `editor.colorscheme`, and `ui.scale`: `editor.font_family` (string), `editor.font_size` (int, range 8..32), `editor.line_endings` (enum: `lf`/`crlf`/`auto`), `editor.save.trim_trailing_whitespace` (bool), `editor.save.ensure_final_newline` (bool), `editor.format_on_save` (bool), `editor.autosave` (enum: `off`/`on_focus_change`/`after_delay`), `editor.hover_delay_ms` (int, range 0..2000), `ui.layout_mode` (enum: `auto`/`regular`/`compact`), `ui.layout_compact_breakpoint_px` (int, range 600..2000), `ui.scrollbar_size` (enum: `compact`/`regular`/`large`), `ui.resize_handle_size` (enum: `compact`/`regular`/`large`), `ui.show_status_bar` (bool), `terminal.shell` (string), `terminal.font_size` (int, range 8..32), `diagnostics.min_severity` (enum: `hint`/`info`/`warning`/`error`).

#### Scenario: New keys round-trip through the registry
- **WHEN** any of the new built-in keys is parsed and re-serialized
- **THEN** `ParseSettingValue` SHALL succeed for every documented value and `SerializeSettingValue` SHALL produce a value that `ParseSettingValue` accepts on the next read

#### Scenario: Hover-delay setting feeds the editor hover popup
- **WHEN** `editor.hover_delay_ms` changes
- **THEN** the editor hover popup SHALL apply the new delay on the next hover, and SHALL NOT require a restart

### Requirement: Overlay Render Path Is Allocation-Bounded

The overlay's render path SHALL build its view model on open and on user input, and SHALL NOT allocate during steady-state idle frames. Re-renders triggered solely by mouse-move within the overlay SHALL reuse the existing view model.

#### Scenario: Idle re-render does not allocate
- **WHEN** the overlay is open, no input arrives, and the cursor moves only within the overlay rectangle
- **THEN** the per-frame render SHALL reuse the cached view model and SHALL NOT allocate new strings or vectors

### Requirement: Settings Guidance Copy Uses Accurate Semantics
Settings overlay helper text SHALL use copy labels that match message intent. Text that describes baseline behavior SHALL be rendered as neutral description (or `Note` where appropriate) and SHALL NOT be labeled as a `Tip`.

#### Scenario: Behavior text is not mislabeled as tip
- **WHEN** a settings row includes explanatory text that tells users how the control works by default
- **THEN** the overlay SHALL render that text without a `Tip` label (or with a neutral `Note` label) and SHALL reserve `Tip` for optional advice only

#### Scenario: Optional advice remains a tip
- **WHEN** helper text recommends an optional workflow optimization
- **THEN** the overlay SHALL label that text as `Tip`
