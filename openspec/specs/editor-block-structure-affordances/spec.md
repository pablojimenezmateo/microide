# editor-block-structure-affordances Specification

## Purpose

Code folding (indent + bracket), fold gutter, fold-aware navigation, sticky scroll, indent guides, render whitespace, and the symbol outline sidebar view. This capability defines the block-structure affordances the host editor presents on top of the existing decorated text-grid pipeline so users can navigate, collapse, and visually parse code structure without leaving the host-owned render path.

## Requirements

### Requirement: Editor Tabs Support Indent And Bracket Code Folding

The editor SHALL compute fold ranges for the active editor tab from two stacked sources, in priority order: (1) bracket-pair ranges when the active language declares a bracket set, computed from a linear scan that skips comment and string regions using the existing per-line `SyntaxState`; (2) indent-block ranges for any region not covered by a bracket pair, where a range opens on a non-blank line whose indent is followed by a strictly-greater indent on the next non-blank line and closes at the next non-blank line whose indent returns to or below the opener's. Fold ranges SHALL be host-owned per tab, recomputed lazily and gated on `(layout_revision, tab_size, language_id)`, and SHALL be available to the renderer and to caret-motion code paths.

#### Scenario: Bracket-defined function body folds
- **WHEN** the editor opens a C-family file containing `void f() {\n  body;\n}`
- **THEN** a fold range SHALL be available that covers the lines from the opening `{` line through the closing `}` line and SHALL be reachable from the gutter fold control on the opener line

#### Scenario: Indent-only fold for an indent-driven language
- **WHEN** the editor opens a Python file with a `def foo():` line followed by indented body lines
- **THEN** an indent-based fold range SHALL be available covering `def foo():` through the last indented body line, even though no bracket pair is declared for that language

#### Scenario: Fold computation is lazy and viewport-bounded
- **WHEN** a 50000-line file is opened and only the first viewport's worth of lines is visible
- **THEN** fold computation SHALL only have resolved fold ranges for the visible viewport and a bounded look-ahead, and SHALL NOT have walked every line of the document on the first frame

#### Scenario: Fold compute respects per-frame budget
- **WHEN** fold computation for a frame would exceed the per-frame budget
- **THEN** the model SHALL return resolved ranges for the lines it could process and SHALL mark the remainder as pending; the renderer SHALL paint the resolved fold gutter marks for that frame and SHALL NOT block the frame on the remainder

### Requirement: Folded Regions Affect Render, Caret Motion, And Soft Wrap Consistently

When a fold range is collapsed, the editor SHALL hide the inner lines from the visible row layout, render a fold-collapsed marker on the opener line, advance Up/Down caret motion across the fold as if it were a single visible row, advance mouse hit-testing as if the inner lines were not present, and skip the inner lines in soft-wrap row layout. Find-in-buffer matches inside a collapsed fold SHALL automatically expand the fold so the match becomes visible.

#### Scenario: Up/Down skips collapsed fold
- **WHEN** a fold range from line 10 to line 20 is collapsed and the caret is on line 9
- **THEN** pressing Down SHALL move the caret to line 21, and the visible viewport row layout SHALL contain no rows for lines 10 through 20

#### Scenario: Click on a fold-collapsed line places caret at the opener
- **WHEN** a fold range is collapsed and the user clicks on the visible opener row
- **THEN** the caret SHALL land on the opener line, and the inner lines SHALL remain hidden until the user explicitly expands the fold

#### Scenario: Search match inside collapsed fold expands the fold
- **WHEN** a fold range is collapsed and a buffer-search match resolves to a line inside that range
- **THEN** the fold SHALL expand and the match SHALL be scrolled into view, with the fold returning to its prior state when the search overlay is dismissed only if the user did not interact with the now-visible content

### Requirement: Fold Gutter Provides Discoverable Controls

The editor SHALL render a fold control in the gutter on every line that opens a fold range, indicating the fold's current state (expanded or collapsed). Clicking the control SHALL toggle the fold; the keyboard SHALL provide commands `editor.fold`, `editor.unfold`, `editor.foldAll`, and `editor.unfoldAll` reachable from the action coordinator and from the menu bar.

#### Scenario: Gutter fold control is visible on opener lines only
- **WHEN** an editor tab is rendered with one or more fold ranges available in the visible viewport
- **THEN** the gutter SHALL show a fold control on each opener line and SHALL NOT show a control on inner or non-folding lines

#### Scenario: Fold All collapses every range
- **WHEN** the user invokes `editor.foldAll`
- **THEN** every available fold range in the document SHALL be collapsed, and `editor.unfoldAll` SHALL restore them

### Requirement: Sticky Scroll Pins Enclosing Context Lines

When the user scrolls past a fold opener line, the editor SHALL paint up to N enclosing parent opener lines as a fixed band at the top of the editor text grid, where N is bounded by configuration (default 3). The pinned context SHALL be derived from the resolved fold-stack at the current scroll position and SHALL be computed by the host view-model builder, not by the render translation unit. Clicking a sticky line SHALL scroll the viewport to that opener.

#### Scenario: Scrolling past opener pins the context
- **WHEN** the editor scrolls down so the opener line of a fold range moves above the viewport
- **THEN** that opener line SHALL be pinned at the top of the editor text grid as a sticky row, and SHALL retain its syntax highlighting and indent guide markers

#### Scenario: Sticky band caps at configured depth
- **WHEN** the caret is inside a deeply nested set of fold ranges and the configuration limit is 3
- **THEN** at most 3 opener lines SHALL be pinned, ordered outermost first

#### Scenario: Sticky scroll content is built by the host view model
- **WHEN** the editor frame is built
- **THEN** the sticky-line stack SHALL be populated by `RenderViewModelBuilder` and SHALL NOT be computed inside an editor render translation unit

### Requirement: Indent Guides Render At Every Indent Step

The editor SHALL render vertical indent guides at every indent step (column = `indent_width * N` for `N >= 1`) for every visible row whose logical content is empty or begins with whitespace at that column. The guide whose column matches the active caret's enclosing block SHALL render with an emphasized color so the caret's parent block is visually obvious. Indent guides SHALL be paintable from the existing decorated text-grid pipeline as one ordered layer; render translation units SHALL NOT compute indent-guide column positions themselves.

#### Scenario: Indent guides at every step
- **WHEN** an editor tab renders content indented to 8 spaces with `indent_width = 4`
- **THEN** vertical guides SHALL appear at columns 4 and 8 for every visible row that is at least that wide and is whitespace-only at those columns

#### Scenario: Active indent guide is emphasized
- **WHEN** the caret is inside a block whose opener indent is column 4
- **THEN** the column-4 guide SHALL render with the active-indent color while the caret remains inside that block

### Requirement: Render Whitespace Mode Reveals Tab And Space Glyphs

The editor SHALL provide a host-owned setting `editor.view.render_whitespace` (default off) that, when on, draws a glyph at every space, tab, and trailing whitespace position in visible rows. Glyphs SHALL render as low-contrast overlays from the same decorated text-grid pipeline; the underlying text content SHALL NOT be modified.

#### Scenario: Render whitespace draws tab and space glyphs
- **WHEN** `editor.view.render_whitespace` is on
- **THEN** every space and tab in visible rows SHALL render with its corresponding low-contrast glyph, and the file content SHALL be unchanged on disk

#### Scenario: Render whitespace toggles cleanly
- **WHEN** the user toggles `editor.view.render_whitespace` off
- **THEN** the next frame SHALL paint without whitespace glyphs and SHALL NOT leave residual glyphs on cached row decorations

### Requirement: Block Structure Hot Paths Stay Within Per-Path Performance Budgets

Every per-frame and per-caret-motion path defined in this spec — fold-range resolution for the visible viewport, sticky-scroll stack resolution, indent-guide compute, render-whitespace overlay compute, and the outline tree refresh — SHALL be cache-keyed on the smallest invariant set that keeps it correct (no broader recompute than necessary), SHALL be viewport-bounded so very large files do not pay full-document cost on first frame, and SHALL avoid per-frame heap allocation by populating pre-sized view-model vectors that are cleared without releasing capacity. Each path SHALL have a dedicated harness scenario committed under `tests/perf/baselines/` so a regression in one path is not masked by improvements in the aggregate `typing_large_file` and `scroll_large_file` scenarios.

#### Scenario: Fold viewport-bounded refresh stays inside the documented budget
- **WHEN** the harness scenario `editor_fold_viewport_refresh` runs against a 50000-line fixture and exercises scroll-driven viewport changes
- **THEN** the run SHALL stay within the committed baseline (P50, P95, max wall-time and allocation count) under the standard tolerances, and a regression beyond tolerance SHALL fail the merge unless the baseline is updated with a `perf-baseline:` justification

#### Scenario: Sticky scroll resolution stays inside the documented budget
- **WHEN** the harness scenario `editor_sticky_scroll_scroll` runs with a 50000-line fixture and a fold depth ≥ 3
- **THEN** the run SHALL stay within the committed baseline under the standard tolerances

#### Scenario: Indent guides do not allocate per frame
- **WHEN** the editor renders consecutive frames with `editor.view.indent_guides.enabled = true` and the viewport unchanged
- **THEN** the indent-guide vector on the editor view model SHALL be reused (capacity preserved across frames), and no new heap allocation SHALL occur for indent-guide payload between frames

#### Scenario: Outline regex fallback completes inside the documented budget
- **WHEN** the harness scenario `editor_outline_regex_fallback` runs against a 50000-line fixture in a language with no active LSP and conventional regex outline hints
- **THEN** the outline build SHALL complete within the committed baseline tolerances and SHALL NOT block the editor input or render path while running

### Requirement: Every Block Structure Affordance Is User-Disable-Able

Every capability defined in this spec — code folding, fold gutter, fold-aware caret motion, sticky scroll, indent guides, render whitespace, and the symbol outline sidebar view — SHALL be individually enable/disable-able by the user through three independent surfaces: (1) a `WorkspaceSettingsRegistry` boolean keyed `editor.fold.enabled`, `editor.fold.sticky_scroll.enabled`, `editor.view.indent_guides.enabled`, `editor.view.render_whitespace`, and `editor.outline.enabled` respectively, persisted at user and project scope with project overriding user; (2) a stable `ActionId` toggle command registered in `WorkspaceCommandRegistry` and bindable through `WorkspaceKeybindingRegistry`; and (3) a checkable menu entry registered in `WorkspaceMenuRegistry` under `View` for presentation toggles. When a capability is disabled the editor SHALL revert to the prior built-in behavior with no residual paint: the fold gutter SHALL paint no fold marks, sticky scroll SHALL paint no sticky band, indent guides SHALL not paint, whitespace glyphs SHALL not paint, and the outline sidebar view SHALL not appear in the registry's ordered view list. The Settings overlay SHALL list every toggle from this spec in a single "Editor → Essentials → Block Structure" group.

#### Scenario: User disables sticky scroll through the View menu
- **WHEN** the user opens `View` and clicks the checkable `Sticky Scroll` entry while it is checked
- **THEN** the entry SHALL become unchecked, `editor.fold.sticky_scroll.enabled` SHALL be set to `false` in user config, the next frame SHALL paint no sticky band, and the underlying fold model SHALL still be available so re-enabling the toggle SHALL restore the band without a tab reload

#### Scenario: User disables folding through a command
- **WHEN** the user invokes `editor.toggleFolding` from the command prompt while folding is enabled
- **THEN** `editor.fold.enabled` SHALL flip to `false`, every collapsed fold in every editor tab SHALL be expanded, the gutter SHALL paint no fold controls, and any `editor.fold.*` follow-up commands SHALL be dispatchable but SHALL be no-ops until folding is re-enabled

#### Scenario: Project override hides the outline view for one project
- **WHEN** project A sets `editor.outline.enabled = false` in project config while user-level config has `editor.outline.enabled = true`, and the user activates project A
- **THEN** the outline sidebar view SHALL not appear in project A's sidebar registry, and switching back to a project without the override SHALL restore the view

### Requirement: Symbol Outline Sidebar View Surfaces Document Symbols

The host SHALL provide a built-in `outline` sidebar view registered in `WorkspaceSidebarRegistry` that, for the active editor tab, displays a tree of `(name, kind, range, selection_range, children)` entries. Outline entries SHALL be sourced from, in priority order: (1) the active LSP server's `textDocument/documentSymbol` response for that tab's language id; (2) a deterministic regex-based fallback declared per language. Clicking an outline node SHALL place the caret at the node's `selection_range` and scroll the editor viewport so that range is centered.

#### Scenario: LSP-backed outline populates from documentSymbol
- **WHEN** an editor tab whose language has an active LSP server is focused, and that server responds to `textDocument/documentSymbol`
- **THEN** the outline view SHALL display the returned symbols as a tree, and SHALL refresh on a debounced timer after edits

#### Scenario: Regex fallback covers languages without an LSP
- **WHEN** an editor tab whose language has no active LSP server is focused, but the language declares a regex outline fallback
- **THEN** the outline view SHALL display the symbols matched by that regex, and the view SHALL indicate that the data is fallback-derived

#### Scenario: Click on outline node moves caret
- **WHEN** the user clicks an outline node
- **THEN** the active caret SHALL move to that node's `selection_range` start and the viewport SHALL scroll so the range is visible
