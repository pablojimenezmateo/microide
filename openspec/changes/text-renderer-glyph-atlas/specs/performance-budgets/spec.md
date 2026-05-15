## ADDED Requirements

### Requirement: Glyph-Atlas-Backed Editor Paint Budget

Once the glyph atlas is enabled by default, the per-iteration `wall_ms` p50 budgets for editor paint scenarios SHALL tighten relative to the pre-atlas baselines committed in `tests/perf/baselines/`. The atlas SHALL be considered the source-of-truth fast path for ASCII text; any regression that pushes wall p50 back above the new ceilings SHALL be treated as a perf-gate failure.

#### Scenario: Whitespace paint stays at or below the new ceiling
- **WHEN** `editor_render_whitespace_paint` runs on `perf-runner-v1` with the atlas enabled
- **THEN** the median `wall_ms` p50 SHALL be at least 30 % below the committed pre-atlas baseline for the same scenario, and the new value SHALL become the regression-gated p50 baseline

#### Scenario: Sticky scroll paint stays at or below the new ceiling
- **WHEN** `editor_sticky_scroll_scroll` runs on `perf-runner-v1` with the atlas enabled
- **THEN** the median `wall_ms` p50 SHALL be at least 20 % below the committed pre-atlas baseline for the same scenario, and the new value SHALL become the regression-gated p50 baseline

#### Scenario: Atlas eviction during a steady-state run is a budget violation
- **WHEN** any committed gate-eligible scenario runs to completion with the atlas enabled
- **THEN** `render.glyph_atlas_evictions` SHALL be 0, and a non-zero value SHALL fail the perf gate regardless of wall-time deltas
