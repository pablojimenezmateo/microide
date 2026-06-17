# Post-`e9a4764` throughput-pass follow-ups + large-file perf validation

- Date: 2026-05-19
- Area: rendering, workspace, performance
- Source: §17 / §17.1–§17.3 and §6; commit `e9a4764` ("perf: land throughput fixes and stabilize
  perf compare")

## Summary

The symmetric throughput seams left open after `e9a4764` (merge scrollbar marker cache, hover-targets
`LineVisualColumnMap`, and fixtures to surface the asymptotic wins) landed by 2026-05-20, and the
large-file/perf validation process debt was narrowed to a measurement discipline.

## Resolution

### §17 throughput-pass follow-ups

Closed on 2026-05-19 except the fixture work in §17.3 (closed 2026-05-20). The three `e9a4764`
changes (merge-conflict grouping → linear pass, compare-surface `LineVisualColumnMap`, compare
scrollbar-marker cache keyed by `model_revision` + track rect) were correct and unit-tested but the
then-current perf fixtures did not exercise their worst cases, so the wall-time delta sat inside the
2σ stdev band.

What was bench-invisible and why (recorded so the investigation is not repeated):
- Merge grouping: `merge_scroll_large_fixture` is a tail-only 1 MB diff producing ~2 `SideChange`s,
  so O(N²) vs O(N) on N=2 is identical at frame scale.
- `LineVisualColumnMap` in `WorkspaceShellCompareRender.cpp`: only fires inside selection/caret
  branches; the burst scenarios scroll without holding a multi-row selection.
- Compare scrollbar marker cache: real but small — eliminating ~30k iterations × 80 frames × 10
  iters saves single-digit µs/frame; the win sits in the noise band.

**§17.1 Merge render per-frame scrollbar marker rebuild** — closed 2026-05-19. `MergeTabState` now
carries `model_revision`, `scrollbar_marker_cache_valid/revision/track/`+ list;
`RefreshMergeTabDerivedState` bumps the revision and invalidates the cache; `DrawMergeScrollbarMarkers`
consumes it gated on revision + track-rect equality (mirrors the compare-side pattern).

**§17.2 Hover targets walking `VisualColumnForTextColumn` three times per hover row** — closed
2026-05-19. `PluginHoverTargetForLine` builds a single `editor::TextLayout::LineVisualColumnMap` for
the hovered line and resolves end-of-line width / start visual / next code-point boundary against it.

**§17.3 Fixtures to surface the existing wins** — closed 2026-05-20. `large_file_open_first_paint`,
`merge_scroll_interleaved_hunks`, and `compare_scroll_selection` run by default with committed local
baselines, covering first paint, interleaved merge hunks, and multi-row compare selection during
sustained scroll.

### §6 Large-file and performance validation (process debt)

Narrowed to an ongoing measurement discipline rather than a discrete fix. The harness includes a
dedicated large-file open-to-first-paint gate plus compare/merge sustained-scroll gates for
interleaved hunks and multi-row compare selection. `tools/perf-compare.py` runs each scenario in both
side orders and merges raw iteration streams before recomputing p50/p95/max (removing single-scenario
fixed-order bias); it is advisory only — `perf-runner-v1` remains the authoritative gate. LTO is
enabled for perf/release builds. **Durable rule:** validate large-file/search/merge/blame/redraw
changes with the startup and runtime profiling docs, not intuition; profile or explicitly accept any
residual regression with data.

References: `dev-docs/performance/startup-tracing.md`, `dev-docs/performance/runtime-profiling.md`,
`dev-docs/performance/performance-findings.md`.
