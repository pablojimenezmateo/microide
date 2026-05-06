## ADDED Requirements

### Requirement: Single-Line Inputs Vertically Center Their Text
Single-line input surfaces (command palette, search, and any caller of `editor::SingleLineEditor` rendered via `WorkspaceShellRenderTextInput`) MUST place their rendered text at `y = rect.y + floor((rect.h - LineHeight()) * 0.5)`. Hard-coded vertical offsets (e.g. `+ 4.0f`) MUST NOT be used.

#### Scenario: Command input is vertically centered
- **WHEN** the command input prompt is rendered with row height `H` and `LineHeight()` returns `L`
- **THEN** the text baseline is positioned at `floor((H - L) / 2)` pixels from the prompt's top edge
- **AND** the visible top and bottom margins above and below the glyphs differ by at most one pixel

#### Scenario: Search input is vertically centered
- **WHEN** the search bar is rendered at any row height
- **THEN** its text uses the same centering formula as the command input
- **AND** matches the centering used by `DrawButtonCentered`

### Requirement: Button And Input Outlines Are Not Painted Over By Text Backgrounds
Buttons and single-line inputs MUST NOT paint a text-background rectangle that occludes any portion of their outline. The text backend's "with background" path MUST be either removed or implemented so that the rendered glyph texture has a transparent background; the visible button/input fill is drawn by the caller before the text and stays untouched by the text texture.

#### Scenario: Button outline is fully visible
- **WHEN** a button is rendered with a fill, an outline, and a label
- **THEN** every pixel of the outline rect remains the outline color after the label is drawn

#### Scenario: Single-line input outline is fully visible
- **WHEN** a single-line input is rendered with a fill, an outline, and an editable string
- **THEN** every pixel of the outline rect remains the outline color after the text is drawn, regardless of the text length or whether a selection is active

### Requirement: Monospace Text Runs Are Column-Stable
For runs whose text is fully ASCII printable, the text backend MUST render at a destination width equal to `text.size() * char_width_pixels`. Splitting a row into multiple runs (e.g. for selection or color spans) MUST produce a total horizontal extent equal to that of the unsplit row over the same character range. Adding a single character to an existing string MUST advance the rendered right edge by exactly one `char_width` regardless of the character's identity.

#### Scenario: Selection split preserves column extent
- **WHEN** a terminal row "Hello world" is first rendered as one run, then re-rendered as `"Hello "` + `"world"` because of a selection
- **THEN** the right edge of the second render coincides with the right edge of the first within one pixel
- **AND** no glyph appears at a column different from its unselected position

#### Scenario: Repeated identical glyph advances by char_width
- **WHEN** the user types `!`, then `!!`, then `!!!` into a single-line input
- **THEN** after each keystroke the cursor's screen x advances by exactly `char_width` pixels relative to the previous keystroke
- **AND** the previously rendered glyphs do not shift horizontally

### Requirement: Source Control Filename And Path Have Distinct Visual Weight
In the Source Control sidebar list, each row's filename SHALL render in `text_primary` and its path SHALL render in `text_muted`, both for selected and unselected rows. The two tokens MUST have visibly different luminance in the active theme.

#### Scenario: Unselected row contrasts filename against path
- **WHEN** a Source Control row is rendered without selection
- **THEN** the filename is drawn in `text_primary`
- **AND** the path is drawn in `text_muted`
- **AND** a sighted user can identify the filename without reading the path

### Requirement: Outgoing Base Reference Is User-Selectable And Persistent
The Source Control "Outgoing" group SHALL expose a base-reference picker. The picker SHALL offer at least: `Auto (resolve base branch)`, `Previous commit (HEAD~1)`, and `Specific ref…` (user-entered string). The chosen value SHALL be persisted per project via `PersistenceService` so it survives restart. When the choice is `Auto`, behavior SHALL be identical to today's `ResolveGitBaseReference`. Older project-state records lacking the field SHALL load with `Auto` as the default.

#### Scenario: User picks previous commit
- **WHEN** the user opens the Outgoing base picker and selects "Previous commit (HEAD~1)"
- **THEN** the Outgoing list refreshes to show files changed since `HEAD~1`
- **AND** the choice is written to project state
- **AND** reopening the project later shows the same Outgoing comparison without re-prompting

#### Scenario: Older project state records load with Auto
- **WHEN** project state from a build prior to this change is opened
- **THEN** the Outgoing base choice is `Auto`
- **AND** the Outgoing list compares against the resolved base branch as before

#### Scenario: Specific ref entry persists exact string
- **WHEN** the user picks "Specific ref…" and enters `origin/release/2026-04`
- **THEN** the Outgoing list compares against that ref
- **AND** subsequent sessions reuse the same ref string without re-asking

### Requirement: LSP Readiness Is Visible And Actions Are Gated
The host SHALL render a passive LSP status indicator (in the bottom panel status row or equivalent existing surface) reflecting at least: `Idle`, `Starting`, `Indexing`, `Ready`, `Failed`. Context-menu entries that depend on the LSP (`Go to Definition`, `Find References`, and any future LSP-driven entries) SHALL be disabled while the state is not `Ready`, with a label that conveys the reason. While an LSP request issued by the user is in flight, the status indicator SHALL show that work is in progress.

#### Scenario: Right-click before LSP is ready
- **WHEN** the user right-clicks in the editor while the LSP state is `Starting` or `Indexing`
- **THEN** the LSP-dependent menu items are disabled
- **AND** the disabled label communicates that the LSP is not ready
- **AND** the status indicator reflects the same state

#### Scenario: Issued request shows in-flight feedback
- **WHEN** the user invokes `Go to Definition` and the LSP is `Ready`
- **THEN** the status indicator shows that an LSP request is in progress until the response arrives or the request times out
- **AND** the rest of the UI remains responsive throughout

### Requirement: Diff Inline-Change Underlines Are Dimmed
Inline word-level diff underlines drawn beneath text in compare and merge surfaces SHALL be rendered at no more than 60% of the source token's alpha. The base color tokens (`diff_added`, `diff_modified`, `diff_deleted`) SHALL remain unchanged for non-underline uses (gutter chips, summary bars).

#### Scenario: Underline alpha is reduced relative to gutter
- **WHEN** the same span is rendered both as an underline beneath text and as a gutter chip
- **THEN** the underline pixel alpha is at most 0.60× the gutter chip's pixel alpha

### Requirement: Title-Bar Double-Click Toggles Maximize
When the host renders its custom chrome title bar, double-clicking anywhere in the title-bar row that is not inside a menu-bar item rect SHALL toggle the window between maximized and restored. The trigger SHALL fire on either an SDL `event.button.clicks == 2` or two single clicks within 500 ms at any positions inside the title-bar row.

#### Scenario: Double-click on empty title-bar area maximizes
- **WHEN** the window is restored and the user double-clicks an area of the chrome title-bar row that is not over a menu-bar item
- **THEN** the window becomes maximized
- **AND** a subsequent identical double-click restores it

#### Scenario: Double-click on a menu-bar item opens the menu
- **WHEN** the user double-clicks on the `File` menu-bar item
- **THEN** the menu opens on the first click and remains open
- **AND** the maximize toggle is not triggered
