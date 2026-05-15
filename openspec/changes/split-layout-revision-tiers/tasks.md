## 1. Tiers + Reason Enum On `TextViewport::DocumentState`

- [x] 1.1 Replace `DocumentState::layout_revision` with four `std::uint64_t` members: `content_revision`, `syntax_revision`, `layout_shape_revision`, `presentation_revision` in `src/editor/TextViewport.h`. Done when the file compiles standalone and the architectural-lint test from task 1.5 fails on a deliberate re-introduction of `layout_revision`.
- [x] 1.2 Add `enum class InvalidationReason { ContentEdit, SyntaxConfig, LayoutShape, Presentation };` to `src/editor/TextViewport.h`, scoped under `TextViewport`. Done when callers can name it without an additional include.
- [x] 1.3 Add typed accessors `content_revision()`, `syntax_revision()`, `layout_shape_revision()`, `presentation_revision()` on `TextViewport` that return the corresponding `DocumentState` counter (or 0 when `document_ == nullptr`). Remove the existing `layout_revision()` accessor.
- [x] 1.4 Rewrite `TextViewport::InvalidateDerivedCaches(start_line)` as `TextViewport::InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line)`. The implementation SHALL bump the tier(s) implied by the reason per the table in `design.md` Decision 2, then run the existing suffix-clearing logic (folding anchor, syntax states, checkpoints, visible/highlight cache prefixes). Done when a unit test that calls `InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0)` bumps `content_revision` and `presentation_revision` exactly once and leaves the other two untouched.
- [ ] 1.5 Extend `tests/ArchitectureInvariantsTests.cpp` with a check that fails on any reintroduction of a single `layout_revision` member or accessor on `TextViewport::DocumentState` (regex against `src/editor/TextViewport.h`). Done when the test green-lights the new four-tier layout and red-lights a deliberate revert.

## 2. Migrate Every Existing Invalidation Call Site

- [x] 2.1 Audit every call to `InvalidateDerivedCaches` in `src/editor/TextViewport.cpp` (sites at 1278, 1442, 1705, 1848 today plus any newly surfaced by the signature change). For each, classify the *cause* into one of the four reasons and update the call. Done when the file compiles and the audit list is captured in the PR description.
- [x] 2.2 Audit every external caller of `InvalidateDerivedCaches` across `src/editor/`, `src/workspace/`, `src/render/`, and `src/project/`. Update each to the typed signature. Done when `git grep 'InvalidateDerivedCaches(' src/` returns only typed-reason calls.
- [x] 2.3 Remove `cached_max_visual_columns_revision_` single-revision dependence: implementation review shows column widths are independent of layout-shape (tab_size already participates as a separate cache-key field that triggers a wipe in `SetTabSize`), so the field was renamed to `cached_max_visual_columns_content_revision_` and tracks `content_revision` only.
- [x] 2.4 Update `highlight_state_revision_` to be a `(content_revision, syntax_revision)` pair and adjust the comparison at TextViewport.cpp:1473/1481/1625/1677 accordingly. Done when a test scroll over a syntax-highlighted buffer leaves `highlight_state_advances_` unchanged.
- [x] 2.5 Update `wrapped_row_layouts_revision_` to track only `layout_shape_revision`. Done when a test that mutates content but not layout shape *still* triggers a rebuild (correctness) and a test that changes only theme does not (the win).

## 3. Renderer-Side Caches

- [x] 3.1 Update `EditorViewRenderer::bracket_match_cache_` (EditorViewRenderer.cpp:322–341) so its `layout_revision` field becomes `content_revision`. Bracket positions depend on bytes only — layout shape does not move brackets. Done when a theme change does not invalidate the bracket-match cache in a unit test.
- [x] 3.2 Update `EditorViewRenderer::indent_guides_cache_` (EditorViewRenderer.cpp:420–437) the same way, plus `layout_shape_revision` because indent guides depend on tab-size geometry. Done when a theme change does not invalidate the indent-guides cache in a unit test.
- [x] 3.3 Update the search-match cache and the per-line view-model cache keys in `src/editor/EditorViewRenderer.h` to embed `content_revision` (search matches scan buffer bytes only). Per-line view-model caches that don't exist outside the search/bracket/indent caches above are not introduced.

## 4. Per-Tier Perf Counters

- [x] 4.1 Add `editor.content_revision_bumps`, `editor.syntax_revision_bumps`, `editor.layout_shape_revision_bumps`, and `editor.presentation_revision_bumps` to `util::PerfCounterId` and `kCounterNames` in `src/util/PerformanceCounters.{h,cpp}`.
- [x] 4.2 Increment the matching counter(s) inside `TextViewport::InvalidateDerivedCaches`, exactly once per tier bumped per call. Done when a unit test asserts the count after a known sequence of invalidations.
- [ ] 4.3 Verify the counters appear under `perf_counters` in a smoke `microide_perf --report-json` run. Done when the JSON contains all four names with non-negative values.

## 5. Scroll-Only Perf Scenario

- [ ] 5.1 Add a `editor_scroll_only_no_content_bump` fixture: a large syntax-highlighted C++ file under `tests/perf/fixtures/editor_scroll_only/` (reuse an existing fixture if size and shape match; otherwise commit a new one with its SHA-256).
- [ ] 5.2 Register the `editor_scroll_only_no_content_bump` scenario in `tests/perf/scenarios/` (or `PerfMain.cpp` per repo convention): open the fixture, `PumpFrames` to warm caches, then drive `Scroll` for N frames. Mark `.smoke = true`.
- [ ] 5.3 Add scenario-level assertions that `editor.content_revision_bumps == 0`, `editor.syntax_revision_bumps == 0`, and `editor.layout_shape_revision_bumps == 0` over the measurement window. Done when the scenario passes locally with the new four-tier code and fails on a deliberate revert to the single-revision design.
- [ ] 5.4 Commit an initial `tests/perf/baselines/editor_scroll_only_no_content_bump.json` from a local run and add the `perf-baseline:` line to the change record.

## 6. Unit-Test Coverage For Each Tier And Each Cache

- [ ] 6.1 Extend `tests/TextViewportTests.cpp` with a fixture that calls each of `InvalidationReason::{ContentEdit, SyntaxConfig, LayoutShape, Presentation}` and asserts the exact set of tier counters that increased. Done when all four reason cases pass.
- [ ] 6.2 Extend `tests/TextViewportTests.cpp` with cross-tier tests asserting: (a) `LayoutShape` does NOT increase `highlight_state_advances_`; (b) `SyntaxConfig` does NOT cause `EnsureWrappedRowLayouts` to rebuild; (c) `Presentation` does NOT invalidate `wrapped_row_layouts_` or `highlight_cache_`.
- [ ] 6.3 Extend `tests/EditorViewRendererTests.cpp` to assert: (a) bracket-match cache survives a theme change; (b) indent-guides cache survives a theme change; (c) per-line view-model cache survives a `SyntaxConfig`-only change but rebuilds on a `Presentation` change.

## 7. Sanitizer And Harness Validation

- [ ] 7.1 Run `cmake --preset microide-asan && ctest --test-dir build/microide-asan --output-on-failure`. Done when 100 % pass.
- [ ] 7.2 Run `cmake --preset microide-ubsan && ctest --test-dir build/microide-ubsan --output-on-failure`. Done when 100 % pass.
- [ ] 7.3 Run `cmake --preset microide-tsan && ctest --test-dir build/microide-tsan --output-on-failure` (after `sudo sysctl vm.mmap_rnd_bits=28`). Done when 100 % pass.
- [ ] 7.4 Run the full `microide_perf --smoke` smoke set locally and capture the JSON. Confirm `editor_sticky_scroll_scroll`, `editor_render_whitespace_paint`, and `editor_indent_guides_paint` p50 are ≤ pre-change values; capture absolute numbers in the PR description.

## 8. Baseline Updates And Budget Tightening

- [ ] 8.1 Update `tests/perf/baselines/editor_sticky_scroll_scroll.json`, `editor_render_whitespace_paint.json`, and `editor_indent_guides_paint.json` to the measured post-split medians. Add `perf-baseline: tiered document revisions landed` to the change record.
- [ ] 8.2 If any other harness scenario shifts outside its per-metric tolerance because of the cache-key changes, update its baseline in the same commit; otherwise leave alone.
- [ ] 8.3 Link the `perf-runner-v1` JSON report (or the local smoke JSON if the harness gate has not yet rerun) from the PR description, with before/after numbers for the three tightened scenarios.

## 9. Docs

- [ ] 9.1 Update `docs/performance-bottleneck-deep-dive-4.md` carry-over table: change "split `document_->layout_revision` into tiered revisions" from "honorable mention" to "done" with a one-line summary of the measured deltas.
- [ ] 9.2 Update `docs/known-tech-debt.md` item 14 status to "closed in `split-layout-revision-tiers`", and leave the entry as historical record.
