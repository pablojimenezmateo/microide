## ADDED Requirements

### Requirement: Fold-Aware Row Mapping Is Viewport-Bounded

The editor SHALL avoid full-document row-map reconstruction for ordinary non-soft-wrap rendering. Fold-aware visual-row lookup SHALL use revision-keyed indexes or interval summaries so first render, scrolling, sticky scroll, fold gutter, hit testing, and caret movement do not scan every document line on each visible-frame rebuild.

#### Scenario: Non-soft-wrap large file renders first viewport without full row-map scan
- **WHEN** a 50000-line file is opened with soft wrap disabled and no collapsed folds in the visible viewport
- **THEN** the first visible editor render SHALL compute rows for the visible viewport and bounded metadata only, and SHALL NOT build one `WrappedRowLayout` entry per document line

#### Scenario: Collapsed folds use interval lookup
- **WHEN** a 50000-line file has collapsed fold ranges outside the visible viewport
- **THEN** determining whether a visible row is hidden SHALL use an indexed collapsed-range lookup instead of scanning all fold ranges for each document line

#### Scenario: Fold gutter lookup is indexed
- **WHEN** the renderer asks whether a visible line opens a fold
- **THEN** the lookup SHALL use an opener-line index keyed by fold revision and SHALL NOT linearly scan all fold ranges per visible row

#### Scenario: Sticky scroll lookup is bounded
- **WHEN** sticky scroll resolves parent opener lines for the current top row
- **THEN** it SHALL use fold indexes or bounded range queries and SHALL NOT iterate every fold range on each scroll frame

### Requirement: Block-Structure View Models Reuse Storage

Fold gutter marks, sticky lines, indent guide runs, occurrence ranges, and whitespace glyph runs SHALL reuse view-model vector capacity across frames when their owning surface is unchanged.

#### Scenario: Consecutive indent-guide frames avoid heap churn
- **WHEN** `editor_indent_guides_paint` renders consecutive scroll frames on the 50000-line fixture
- **THEN** indent-guide payload vectors SHALL be cleared without releasing capacity and per-frame allocation count SHALL stay within the committed isolated baseline

#### Scenario: Render-whitespace frames avoid duplicate scans
- **WHEN** `editor_render_whitespace_paint` runs with unchanged visible rows
- **THEN** whitespace glyph data SHALL be built once per relevant view-model revision and reused by the renderer rather than recomputed independently in render translation units

### Requirement: Block-Structure Perf Targets Are Measured After Isolation

The block-structure scenarios SHALL be re-baselined only after harness state isolation is in place, and implementation changes SHALL cite before and after runs for adjacent editor paths.

#### Scenario: Block-structure optimization includes adjacent checks
- **WHEN** code changes touch fold mapping, row layout, indent guide, sticky scroll, or render-whitespace paths
- **THEN** the change SHALL run `editor_fold_recompute`, `editor_fold_viewport_refresh`, `editor_sticky_scroll_scroll`, `editor_indent_guides_paint`, `editor_render_whitespace_paint`, `typing_large_file`, and `scroll_large_file` before and after the change
