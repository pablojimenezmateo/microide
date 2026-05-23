## Why

Every edit and every scroll currently bumps a single `document_->layout_revision`,
which is the cache-invalidation key for four logically independent caches in
`TextViewport` and `EditorViewRenderer`:

1. wrapped-row layouts (`wrapped_row_layouts_`),
2. visible-line + syntax-highlight caches (`visible_line_cache_`, `highlight_cache_`,
   `line_highlight_states_`, `highlight_checkpoints_`),
3. bracket-match and indent-guide caches in `EditorViewRenderer`,
4. width / texture caches in `TextRenderer` (color theme + font id, not edits).

Because there is one revision counter, a one-character insertion past the visible
region — or a soft-wrap toggle that does not mutate any line — invalidates the
suffix of every derived cache. The round-4 deep-dive added lazy invalidation
*cursors* (`line_highlight_states_valid_through_`, `highlight_checkpoints_valid_through_`)
that made the reset O(1), but **readers still recompute** because the revision
they compare against still moves. With the glyph atlas closed as a dead end
(`dev-docs/project/known-tech-debt.md` #13), this is now the single biggest remaining
editor-paint win on the board; `dev-docs/project/known-tech-debt.md` #14 estimates 10–30 %
wall-time reduction across editor scenarios.

## What Changes

- Replace `TextViewport::Document::layout_revision` with four tiered counters on
  the same struct: `content_revision`, `syntax_revision`, `layout_shape_revision`,
  `presentation_revision`. The old single counter is removed (no compatibility
  shim — caches re-key on the minimum tier they actually depend on).
- Replace the single `InvalidateDerivedCaches(start_line)` entry point with a
  reason-typed call: `InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line)`
  where `InvalidationReason` is an enum naming exactly which tier (or tiers)
  this mutation must bump. Every existing caller is updated to pass its actual
  reason; reviewers can audit the call site list.
- Re-key each derived cache on its minimum tier set:
  - `wrapped_row_layouts_` → `layout_shape_revision` only.
  - `visible_line_cache_` → `content_revision` + `presentation_revision`.
  - `highlight_cache_` / `line_highlight_states_` / `highlight_checkpoints_`
    → `content_revision` + `syntax_revision`.
  - Bracket-match + indent-guide caches in `EditorViewRenderer` → `content_revision`
    + `layout_shape_revision`.
  - `cached_max_visual_columns_revision_` → `content_revision` + `layout_shape_revision`.
- Add the per-tier counters that already exist as concept-only today:
  `editor.content_revision_bumps`, `editor.syntax_revision_bumps`,
  `editor.layout_shape_revision_bumps`, `editor.presentation_revision_bumps`,
  plus a `editor.cache_hits_after_scroll` counter on `wrapped_row_layouts_` so
  the scroll-only fixture below has something to assert.
- Add an `editor_scroll_only_no_content_bump` perf scenario asserting that scrolling
  N frames produces `content_revision_bumps == 0` (today this counter would tick
  up on every scroll because of unrelated invalidations).
- Update the architectural-lint test to forbid reintroducing a single
  `layout_revision` member on `TextViewport::Document` or its alias.

## Capabilities

### New Capabilities
- `tiered-document-revisions`: contract for the four-tier revision model on
  `TextViewport::Document`. Owns the tier definitions, the invariant that each
  derived cache keys on the minimum tier set it depends on, the
  `InvalidateDerivedCaches(InvalidationReason, start_line)` shape, and the
  assertable per-tier perf counters.

### Modified Capabilities
- `performance-budgets`: add a scenario-level budget for
  `editor_scroll_only_no_content_bump` (`content_revision` SHALL NOT bump during
  pure scrolling) and tighten the p50 wall budgets for `editor_sticky_scroll_scroll`,
  `editor_render_whitespace_paint`, and `editor_indent_guides_paint` once the
  tiers are live and the harness has measured the new ceiling.
- `performance-harness`: register the new `editor_scroll_only_no_content_bump`
  scenario and add the four `*_revision_bumps` counters plus
  `editor.cache_hits_after_scroll` to the reportable counter set.

## Impact

- Code: `src/editor/TextViewport.{h,cpp}` (revision struct + invalidation
  fan-out), `src/editor/EditorViewRenderer.{h,cpp}` (bracket / indent-guide /
  visible-line cache keys), `src/editor/FoldingModel.h` (its own
  `layout_revision` already exists and is independent — leave as is; verify by
  grep that nothing in `Document` re-aliases it),
  `src/util/PerformanceCounters.{h,cpp}` (new counters),
  `tests/perf/PerfMain.cpp` + `tests/perf/fixtures/` (new scenario),
  `tests/ArchitectureInvariantsTests.cpp` (forbid the single-revision regression),
  `tests/TextViewportTests.cpp` / `tests/EditorViewRendererTests.cpp` (tier-key
  unit coverage).
- APIs: `TextViewport::InvalidateDerivedCaches` signature changes
  (`InvalidationReason` enum added). Every internal caller is updated in the
  same change; this is not a public API surface for plugins or external embedders.
- Dependencies: none.
- Sanitizers: no new threads, no new shared state — change runs through the
  standard ASAN/UBSAN/TSAN presets in `AGENTS.md`.
- Performance harness: existing baselines in `tests/perf/baselines/` for the
  affected scroll/paint scenarios will need refreshing once the tiers land; the
  change record will include the required `perf-baseline:` line. A scroll-only
  scenario is added (no existing baseline; first run defines it).
- Risk: medium-wide surface (every `InvalidateDerivedCaches` caller and every
  cache that compared against `layout_revision`), but each individual change is
  mechanical. Risk vector is forgetting to bump a tier where the old code
  bumped the single revision — caught by unit tests that exercise each mutation
  shape and by the architectural-lint regression guard.
