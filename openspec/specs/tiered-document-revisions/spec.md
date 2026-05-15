# tiered-document-revisions Specification

## Purpose
TBD - created by archiving change split-layout-revision-tiers. Update Purpose after archive.
## Requirements
### Requirement: Four Tiered Document Revisions

`TextViewport::DocumentState` SHALL expose exactly four cache-invalidation
counters — `content_revision`, `syntax_revision`, `layout_shape_revision`, and
`presentation_revision` — and SHALL NOT expose a combined or aliased
`layout_revision` counter. Each counter is an unsigned 64-bit integer that
monotonically increases.

#### Scenario: The single legacy counter is gone
- **WHEN** the source tree is searched for `DocumentState::layout_revision` or
  any alias / reference / typedef that names a single combined revision on
  `TextViewport::DocumentState`
- **THEN** no such member, alias, or accessor SHALL exist, and the
  architectural-lint test SHALL fail any change that re-introduces one

#### Scenario: All four tiers are reachable from the viewport
- **WHEN** test code reads each tier through `TextViewport`
- **THEN** typed accessors for `content_revision()`, `syntax_revision()`,
  `layout_shape_revision()`, and `presentation_revision()` SHALL return the
  corresponding `DocumentState` counter

### Requirement: Reason-Typed Invalidation Entry Point

`TextViewport::InvalidateDerivedCaches` SHALL require a typed
`InvalidationReason` and SHALL bump only the tier(s) implied by that reason.
The reason-blind `InvalidateDerivedCaches(start_line)` signature SHALL NOT
exist.

`InvalidationReason` is an `enum class` with exactly the values
`ContentEdit`, `SyntaxConfig`, `LayoutShape`, and `Presentation`. The
implementation maps reasons to tier bumps as follows:

- `ContentEdit` → bumps `content_revision` AND `presentation_revision`.
- `SyntaxConfig` → bumps `syntax_revision` AND `presentation_revision`.
- `LayoutShape` → bumps `layout_shape_revision` AND `presentation_revision`.
- `Presentation` → bumps `presentation_revision` only.

#### Scenario: Edit bumps content and presentation only
- **WHEN** a unit test calls a content-mutating operation (insert / delete /
  paste / undo / redo) on `TextViewport`
- **THEN** `content_revision` SHALL increase exactly once, `presentation_revision`
  SHALL increase exactly once, and `syntax_revision` and `layout_shape_revision`
  SHALL be unchanged

#### Scenario: Soft-wrap toggle bumps layout shape and presentation only
- **WHEN** a unit test toggles soft wrap, changes tab size, collapses or
  expands a fold, or otherwise calls into `TextViewport` with reason
  `LayoutShape`
- **THEN** `layout_shape_revision` SHALL increase exactly once,
  `presentation_revision` SHALL increase exactly once, and `content_revision`
  and `syntax_revision` SHALL be unchanged

#### Scenario: Theme change bumps syntax and presentation only
- **WHEN** a unit test changes the highlight contract / color theme through
  `TextViewport` with reason `SyntaxConfig`
- **THEN** `syntax_revision` SHALL increase exactly once,
  `presentation_revision` SHALL increase exactly once, and `content_revision`
  and `layout_shape_revision` SHALL be unchanged

#### Scenario: Decoration overlay change bumps presentation only
- **WHEN** a unit test mutates only decoration / overlay state with reason
  `Presentation`
- **THEN** `presentation_revision` SHALL increase exactly once and
  `content_revision`, `syntax_revision`, and `layout_shape_revision` SHALL be
  unchanged

### Requirement: Caches Key On Minimum Tier Set

Each derived cache in `TextViewport` and `EditorViewRenderer` SHALL store, as
part of its cache key, only the tiers it actually depends on, and SHALL
rebuild only when one of those tiers changes. A tier the cache does not
depend on SHALL NOT cause that cache to rebuild.

Required minimum tier sets:

- `wrapped_row_layouts_` → `layout_shape_revision`.
- `visible_line_cache_` → `content_revision`, `presentation_revision`.
- `highlight_cache_`, `line_highlight_states_`, `highlight_checkpoints_` →
  `content_revision`, `syntax_revision`.
- `cached_max_visual_columns_` → `content_revision`, `layout_shape_revision`.
- `EditorViewRenderer::bracket_match_cache_` → `content_revision`,
  `layout_shape_revision`.
- `EditorViewRenderer::indent_guides_cache_` → `content_revision`,
  `layout_shape_revision`.
- Per-line view-model cache key in `EditorViewRenderer` → `content_revision`,
  `layout_shape_revision`, `presentation_revision`.

#### Scenario: Scroll does not invalidate the highlight cache
- **WHEN** a test scrolls the viewport without mutating content, syntax, or
  layout shape
- **THEN** `highlight_cache_` lookups SHALL be served from the prior cache
  state, and `highlight_state_advances_` SHALL NOT increase

#### Scenario: Theme change does not invalidate wrapped-row layouts
- **WHEN** a test changes the color theme with reason `SyntaxConfig`
- **THEN** the next call to `EnsureWrappedRowLayouts` SHALL return the
  pre-existing layouts without rebuilding

#### Scenario: Soft-wrap toggle does not invalidate the highlight cache
- **WHEN** a test toggles soft wrap with reason `LayoutShape`
- **THEN** `highlight_cache_` entries SHALL remain valid and SHALL NOT be
  recomputed on the next read

#### Scenario: Decoration update does not invalidate wrapped-row layouts or highlights
- **WHEN** a test mutates decoration overlay state with reason `Presentation`
- **THEN** `wrapped_row_layouts_` and `highlight_cache_` SHALL remain valid
  and SHALL NOT be recomputed on the next read

### Requirement: Per-Tier Counters Are Reportable

`util::PerformanceCounters` SHALL expose `editor.content_revision_bumps`,
`editor.syntax_revision_bumps`, `editor.layout_shape_revision_bumps`, and
`editor.presentation_revision_bumps`, each incremented exactly once per
matching tier bump.

#### Scenario: Each tier bump increments its counter exactly once
- **WHEN** a unit test calls `InvalidateDerivedCaches(reason, …)` with a
  given reason
- **THEN** the corresponding `*_revision_bumps` counter(s) SHALL increase
  exactly by the number of tiers that reason bumps, with no over- or
  under-counting

#### Scenario: Counters appear in the harness JSON
- **WHEN** the perf harness runs a scenario that exercises the editor
- **THEN** the four `editor.*_revision_bumps` counters SHALL appear under
  `perf_counters` in the scenario's `--report-json` output

### Requirement: Fold Model Revision Is Independent

`FoldingModel::Snapshot::layout_revision` SHALL remain owned by
`FoldingModel` and SHALL NOT be merged with or aliased to any tier on
`TextViewport::DocumentState`.

#### Scenario: Fold model revision is unchanged by this work
- **WHEN** the source tree is searched after this change lands
- **THEN** `FoldingModel::Snapshot::layout_revision` SHALL still exist on
  the fold model, and no `TextViewport` code SHALL alias to it

