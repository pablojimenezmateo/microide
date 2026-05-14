# Performance Bottleneck Deep Dive

Date: 2026-05-14

This document records a codebase-level performance investigation focused on making MicroIDE
substantially faster, not preserving compatibility with existing internal boundaries. The findings
come from local advisory perf runs, existing baseline files, and inspection of render, editor,
terminal, search, git, compare, and plugin paths.

The local measurements below used the existing perf harness under SDL dummy video. They are useful
for ranking bottlenecks, but the authoritative gate remains `perf-runner-v1` and the documented
baseline workflow in `docs/perf-harness.md`.

## Implemented Tooling (2026-05-14)

The following P0 measurement tooling is now implemented in code:

1. Phase allocation metrics in perf scenarios:
   `ScenarioContext::Measure(...)` now records per-phase wall time plus allocations/frees and
   bytes allocated/freed.
2. Global perf counters with per-iteration deltas in perf JSON:
   `util::PerformanceCounters` now tracks frame prep, editor invalidation/layout rebuild, terminal
   snapshot/trim, search/file-finder, and text-renderer cache counters.
3. Perf report output wiring:
   `microide_perf` JSON now includes `phase_metrics` and `perf_counters` per iteration, while
   text reports include top counter totals per scenario.
4. Baseline inventory drift guard:
   `microide_perf` now fails early if `tests/perf/baselines/*.json` contains scenario baselines
   that are not registered in the harness.
5. Baseline inventory cleanup:
   stale baselines `editor_outline_regex_fallback` and `editor_outline_lsp_refresh` were removed
   because no matching scenarios are registered.

## Implemented Runtime Optimizations (2026-05-14, Ongoing Slices)

The following high-priority frame-path optimizations are now implemented:

1. Settings generation gate for editor preference reapply:
   per-frame `ApplyEditorPreferencesToAllTabs()` work is skipped when effective settings have not
   changed.
2. Status bar language cache:
   language detection and language-contract-derived language labels are cached by active file and
   invalidated only on relevant editor/tab transitions.
3. Diagnostics severity aggregates:
   `DiagnosticsStore` now maintains cached severity counts and a revision counter so status-bar
   problems indicators avoid per-frame `SnapshotAll()` materialization.
4. Status bar repository validity cache:
   fallback repository validity probing is cached by normalized project root to avoid repeated
   per-frame `GitRepository(...).IsValid()` filesystem checks.
5. LSP status snapshot reuse in status bar and bottom panel:
   status-bar LSP text/tooltip now reuse a single readiness snapshot per refresh, and bottom-panel
   LSP label rendering uses non-starting status lookup (`ensure_started=false`) to avoid render-path
   startup/probe work.

## Executive Summary

The largest remaining costs are broad invalidation, repeated per-frame work, and large copies. The
high-value fixes are architectural rather than local micro-optimizations.

1. Frame preparation still does work that should be event-driven. The worst offender is applying
   editor preferences to every tab every frame.
2. `TextViewport` invalidates and rebuilds document-scale derived state for small edits, especially
   near the top of large files.
3. Multi-caret and grouped edit paths still copy either the entire document or a broad contiguous
   slice between far-apart carets.
4. Editor rendering builds expensive view models more than once per pane per frame, with a second
   pass just to draw scrollbars.
5. Text rendering relies heavily on whole-string measurement and texture caches, which churn on
   code-like content.
6. Terminal scrollback and visible-line snapshots allocate heavily during output and render.
7. Search and file-finder paths copy full indexes and lowercase full lines/paths where shared
   snapshots and allocation-free scans are needed.
8. Git/status-bar refresh has main-thread fallback and per-frame filesystem/subprocess risk that
   should be removed from shell preparation entirely.

The recommended path is to first improve measurement so the perf harness can attribute allocations
to phases, then remove redundant per-frame work, then rewrite the editor data structures that cause
document-scale invalidation.

## Measurement Snapshot

These numbers are from local advisory runs against the existing `build/microide-perf-make` binary.
They should not be treated as new baselines.

| Scenario | Local p50 | Local p95 | Local p50 allocations | Primary signal |
| --- | ---: | ---: | ---: | --- |
| `editor_smart_indent_typing` | 4263.75 ms | 4343.44 ms | 3.67M | Small newline/undo near top of 50k-line file is document-scale. |
| `editor_auto_close_typing` | 4380.49 ms | 4448.75 ms | 0.73M | Pair insert/undo pays broad edit invalidation. |
| `editor_fold_recompute` | 1376.93 ms | 1419.47 ms | 21.85M | Fold/edit interaction rebuilds too much state. |
| `editor_fold_viewport_refresh` | 935.81 ms | 949.22 ms | 1.55M | Fold rendering/view-model refresh is too expensive. |
| `editor_sticky_scroll_scroll` | 978.12 ms | 1069.95 ms | 6.81M | Sticky scroll/render pass allocates heavily. |
| `editor_render_whitespace_paint` | 926.91 ms | 943.63 ms | 1.42M | Whitespace model/render path is per-frame heavy. |
| `editor_indent_guides_paint` | 702.72 ms | 712.93 ms | 1.19M | Indent guide paint still scans/allocates too broadly. |
| `editor_surround_multi_caret` | 553.76 ms | 596.96 ms | 17.64M | Multi-caret surround copies broad slices. |
| `editor_mouse_selection_drag` | 187.60 ms | 282.70 ms | 8.77M | Scenario total includes setup; measured drag phase was below 1 ms. |
| `multi_tab_cycle` | 404.46 ms | 405.79 ms | 2.94M | Tab cycle triggers repeated frame/tab-wide work. |
| `terminal_scroll_long_output` | 96.75 ms | 105.39 ms | 2.09M | Terminal output/render snapshots allocate heavily. |
| `window_resize_stress` | 49.70 ms | 60.41 ms | 82k | Less urgent than editor/terminal paths. |
| `typing_large_file` | 4.72 ms | 20.93 ms | 22k | The basic single-character path is comparatively healthy. |
| `scroll_large_file` | 5.19 ms | 29.27 ms | 21k | Baseline scroll is comparatively healthy without extra surfaces. |

The perf harness currently reports scenario-level allocations but not phase-level allocations.
That matters because `editor_mouse_selection_drag` shows 8.77M scenario allocations while the
explicit `mouse_selection_drag.160_moves` phase took under 1 ms. Phase allocations are needed before
using allocation totals to judge individual interactions.

## Finding 1: Frame Preparation Reapplies Settings To Every Tab

Relevant code paths:

- `src/workspace/WorkspaceShellRenderFrame.cpp`
- `src/workspace/WorkspaceShellSettingsOverlay.cpp`
- `src/workspace/WorkspaceShellEditor.cpp`
- `src/workspace/RenderViewModelBuilder.cpp`

`PrepareFrameOnce()` calls `ApplyLiveSettings()` every frame. `ApplyLiveSettings()` calls
`ApplyEditorPreferencesToAllTabs()`, which iterates the welcome surface and every editor tab. For
each viewport, `ApplyEditorPreferences()` re-reads settings, updates multiple viewport flags,
detects filetype from path and lines, and rebuilds the language contract view.

That is the wrong direction of ownership. Settings are low-frequency state changes; frame
preparation is the hottest shell path. The current design turns a frame into an implicit
all-open-tabs settings reconciliation pass.

This likely explains the current `multi_tab_cycle` allocation regression. The local run measured
about 2.94M p50 allocations for `multi_tab_cycle`, while existing baselines in the repo are much
lower. Even if some of that is setup, the code path is structurally wrong for an IDE that should be
fast with many open tabs.

Rewrite plan:

1. Add an immutable effective editor settings snapshot with a monotonically increasing generation.
2. Apply editor preferences only when that generation changes, when a tab opens, when language
   detection changes, or when a project/session is activated.
3. Cache each viewport's applied settings generation and language contract generation.
4. Move language detection and language contract building out of frame prep.
5. Add a perf counter for `ApplyEditorPreferencesToAllTabs()` and assert it is zero during steady
   frame prep.
6. Replace render-path parsing such as `ParseStickyScrollMaxDepthSetting()` with cached typed
   settings using `util::ParseInt`, not exception-based parsing.

Expected result:

- `multi_tab_cycle` should stop scaling with number of open tabs during frame preparation.
- Idle and caret-only frames should do no tab-wide settings work.
- Render surface state becomes easier to reason about because settings updates have explicit
  causes.

## Finding 2: TextViewport Small Edits Rebuild Document-Scale Derived State

Relevant code paths:

- `src/editor/TextViewport.cpp`
- `src/editor/TextViewport.h`
- `src/editor/FoldingModel.cpp`
- editor perf scenarios in `tests/perf/PerfScenarios.cpp`

Small edits call `ApplyRangeEdit()`, then `ApplyHistoryEntry()`, then broad follow-up work:

- `RefreshEncoding()` scans the document.
- `InvalidateDerivedCaches(start_line)` erases and resets cache state from the changed line to EOF.
- `EnsureCursorVisible()` asks for cursor visual row.
- Cursor visual row resolution can force `EnsureWrappedRowLayouts()`.
- `EnsureWrappedRowLayouts()` rebuilds full document row layout when the layout revision changes.

For an edit at line 5 in a 50k-line file, this is effectively O(document size) per insert and per
undo. That matches the local measurements for `editor_smart_indent_typing` and
`editor_auto_close_typing`, both around 4.3 seconds p50.

The non-soft-wrap case is especially important. When soft wrap is off and there are no hidden
folds, visual row equals document line. A full `wrapped_row_layouts_` vector rebuild is unnecessary
for many common operations.

Rewrite plan:

1. Split revisions into content revision, syntax revision, layout-map revision, and viewport
   presentation revision instead of one broad layout revision.
2. Add a non-soft-wrap fast path where visual row is computed directly from line index unless folds
   are active.
3. Move fold-aware visual row mapping into `FoldingModel` or a dedicated row-index structure with
   prefix counts and local updates.
4. Replace `RefreshEncoding()` on every edit with incremental encoding state or deferred
   recomputation on save/load/large paste.
5. Make syntax invalidation range-based. Dirty checkpoints should start from the minimal safe
   lexical boundary, not blindly reset every line to EOF.
6. Add regression counters for wrapped-row rebuilds, encoding rescans, syntax checkpoint resets,
   and full-document cache invalidations.

Expected result:

- Single-line edits in large files should be near constant time in the no-soft-wrap/no-fold case.
- `editor_smart_indent_typing`, `editor_auto_close_typing`, and `editor_fold_recompute` should
  drop by orders of magnitude.
- The same data structure work will also improve caret motion, diagnostics placement, sticky
  scroll, and scrollbar calculations.

## Finding 3: Multi-Caret And Grouped Edits Still Copy Too Much

Relevant code paths:

- `src/editor/TextViewport.cpp`
- `src/editor/TextViewportHistory.cpp`
- `src/editor/TextViewportUndo.cpp`

Several multi-caret operations still copy `document_->lines` wholesale before applying changes.
Other paths avoid a full document copy but capture one broad contiguous slice from the first touched
line to the last touched line. That still fails when carets are far apart.

`editor_surround_multi_caret` is the clearest signal. The scenario uses a small number of carets
spread across thousands of lines. The local run measured about 17.64M p50 allocations and nearly a
second of aggregate allocated memory movement across two iterations. The edit itself is small; the
history and invalidation representation is not.

Rewrite plan:

1. Introduce a batch edit engine that accepts sorted disjoint edits and applies them descending by
   position.
2. Store undo as a compact multi-span patch list, not a before/after document snapshot and not a
   broad contiguous slice.
3. Invalidate derived caches once per batch using the minimum changed line and structural line
   delta.
4. Remove full-document snapshots from multi-caret insert, backspace, delete-forward, and line
   deletion paths.
5. Replace `BuildHistoryEntryForDocumentChange()` usage in hot edit paths with explicit affected
   ranges.
6. Add tests proving far-apart carets allocate O(caret count plus edited bytes), not O(distance
   between carets) and not O(file size).

Expected result:

- `editor_surround_multi_caret` should become a small bounded edit.
- Multi-caret typing and deletion should stop punishing large files.
- Undo history becomes a better product primitive for future merge-aware or structured edit
  features.

## Finding 4: Editor Rendering Builds View Models More Than Once Per Pane

Relevant code paths:

- `src/workspace/WorkspaceShellRenderFrame.cpp`
- `src/workspace/RenderViewModelBuilder.cpp`
- `src/editor/EditorViewRenderer.cpp`

The active editor render path currently builds editor presentation state in multiple passes. It
computes metrics, builds a view model, sometimes recomputes metrics after sticky scroll rows are
known, builds again, then later loops over panes again to compute scrollbar layout and may build a
reduced editor view model just for scrollbar inputs.

That duplicates work that is already expensive:

- `visual_line_count()` and wrapped row layout access.
- folding model freshness checks.
- sticky scroll row computation.
- occurrence and whitespace collection.
- editor metrics derivation.
- scrollbar marker preparation.

The paint-oriented scenarios show the result: sticky scroll, fold refresh, whitespace rendering,
and indent guides all run hundreds of milliseconds locally with large allocation counts.

Rewrite plan:

1. Create an `EditorPaneRenderPlan` built once per pane per frame.
2. Store metrics, sticky rows, the final editor view model, scrollbar layout inputs, gutter state,
   diagnostics spans, blame state, and text input geometry in that plan.
3. Draw editor content, gutters, overlays, and scrollbars from the same plan.
4. Avoid build-then-rebuild for sticky scroll by computing sticky rows before the final view model,
   or by making sticky rows a cheap field in the first pass.
5. Add a debug/perf assertion that no pane builds more than one final editor view model per frame.

Expected result:

- Paint scenarios should become primarily visible-row work rather than repeated pane preparation.
- Scrollbar rendering stops pulling editor model construction into a second pass.
- The render loop becomes easier to profile because each pane has one preparation boundary.

## Finding 5: Render Model And Text Backend Allocate On Hot Paths

Relevant code paths:

- `src/workspace/RenderViewModelBuilder.cpp`
- `src/editor/EditorViewRenderer.cpp`
- `src/editor/DecoratedTextGridRenderer.cpp`
- `src/render/TextRenderer.cpp`
- `src/render/SdlTtfTextBackend.cpp`

The editor render path avoids some obvious allocations with scratch buffers, but hot paths still
materialize strings, vectors, or copied caret collections.

Observed risks:

- `TextViewport::secondary_carets()` returns a vector copy for rendering.
- Whitespace glyphs are collected as a full visible-window vector, then rendering loops through the
  collection per row instead of using row-indexed spans.
- Occurrence collection builds visible-line sets and range vectors.
- Whole-string width and texture caches are keyed by copied strings.
- SDL_ttf texture caching works poorly for code-like content where line/run strings are often
  unique while scrolling.
- Fallback sidebar/search strings are materialized in view-model building.

Whole-string texture caches are a reasonable first implementation, but they are not enough for a
fast software-rendered IDE. A code editor needs line-local layout reuse and glyph or cluster reuse
that survives scrolling through unique code.

Rewrite plan:

1. Return caret spans/views for render instead of copying secondary caret vectors.
2. Make whitespace, occurrence, diagnostics, and indent guide data row-indexed in the view model.
3. Cache line layout by viewport revision, line number, horizontal window, font, theme, and syntax
   generation.
4. Add a glyph or cluster atlas for common editor and terminal text rather than relying only on
   whole-string textures.
5. Add text renderer stats to perf output: width-cache hits/misses, texture-cache hits/misses,
   evictions, rendered string count, glyph atlas hits/misses.
6. Move all render-path string assembly into `RenderViewModelBuilder` or earlier service state,
   and keep render translation units free of hot-path string creation.

Expected result:

- Scrolling through unique code should not churn thousands of one-off text textures.
- Paint scenarios should be gated by visible glyph count, not repeated string materialization.
- Renderer regressions become visible in perf reports instead of requiring manual tracing.

## Finding 6: Terminal Output And Rendering Allocate Heavily

Relevant code paths:

- `src/terminal/TerminalSession.cpp`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp`

`terminal_scroll_long_output` measured about 2.09M p50 allocations locally. Some of the wall time
includes fixed scenario waits, but the allocation count is still too high.

Terminal risks:

- Visible line snapshots copy `TerminalLine` and cell vectors when the snapshot generation changes.
- Scrollback trimming uses vector erase from the front, which moves retained lines.
- Rendering builds `std::string` run text for foreground runs every frame.
- Cursor drawing materializes a string for the cell under the cursor.
- Per-cell UTF-8 text is stored as `std::string`, which is correct for Unicode but expensive if not
  paired with line/run-level caching.

Rewrite plan:

1. Replace vector-backed scrollback trimming with a ring, deque, or paged line store.
2. Expose immutable visible line spans keyed by generation instead of copying visible cells into a
   snapshot for render.
3. Maintain per-line render runs cached by line generation, selection generation, and cursor state.
4. Reuse per-terminal scratch strings for run assembly with reserved capacity.
5. Use the same glyph/cluster atlas strategy as the editor for terminal text.
6. Add terminal-specific perf counters: copied visible cells, trimmed lines, run strings built,
   cached line-run hits, scrollback page drops.

Expected result:

- Sustained terminal output becomes append/drop work, not vector shifting and visible-cell copying.
- Terminal rendering allocation count should fall from millions to a small visible-window bounded
  number.

## Finding 7: Search And File Finder Copy Full Indexes

Relevant code paths:

- `src/project/FileIndex.cpp`
- `src/project/FileFinder.cpp`
- `src/project/ProjectSearchService.cpp`
- `src/workspace/WorkspaceProjectSearchRuntime.cpp`

The project file index is convenient but copy-heavy. `Snapshot()` returns a full vector of
`ProjectFile`. The file finder then duplicates path strings and lowercase strings. Query refresh
scans all cached entries and full-sorts matches.

Project search has similar allocation pressure:

- Case-insensitive literal search lowercases each candidate line into a new string.
- Candidate paths are materialized with `relative_path.string()`.
- Progress updates are frequent and event-based.
- Active result consumption snapshots all current results into shell state.

This is acceptable at small project sizes but will not meet the product goal for large repositories.

Rewrite plan:

1. Make `FileIndex` expose immutable generation snapshots via shared ownership or RCU-style handles.
2. Let file finder cache index-owned string views or stable path IDs instead of duplicating every
   path on refresh.
3. Use top-K selection for visible file-finder results rather than sorting every match.
4. Add incremental query refinement when a query extends the previous query.
5. Implement allocation-free ASCII case-insensitive literal scan for project search.
6. Coalesce progress events and deliver result deltas instead of full snapshots.
7. Add a 100k-file perf fixture for file finder and project search.

Expected result:

- File finder latency scales with candidates examined and result count, not repeated full copies.
- Project search spends CPU on file scanning, not line/path allocation.
- Large repositories become a first-class performance target.

## Finding 8: Git And Status Bar Refresh Are Not Fully Event-Driven

Relevant code paths:

- `src/workspace/WorkspaceShellChrome.cpp`
- `src/workspace/WorkspaceSidebarCoordinator.cpp`
- `src/workspace/WorkspaceSidebarCoordinatorRefresh.cpp`
- `src/project/GitRepository.cpp`

`RefreshStatusBar()` runs during frame preparation. It builds strings and reads editor/project/git
state every frame. It also checks repository validity and may resolve branch labels when cached
state is missing. That is too much work for a frame path, and any filesystem or subprocess fallback
there risks UI latency spikes.

The sidebar coordinator has an async git refresh path, but `RefreshGit()` still contains a
synchronous fallback when no async snapshot is available. That should not exist on the main shell
path.

Rewrite plan:

1. Make status bar segments event-driven.
2. Update line/column only on cursor movement or active editor change.
3. Update language only on tab open, language detection change, or settings generation change.
4. Update encoding and line endings on load/save/edit events that actually affect them.
5. Update git branch and dirty/conflict state only from async git snapshots.
6. Remove synchronous git fallback from sidebar refresh; render stale or loading state instead.
7. Add a lint rule forbidding git subprocess collection and repository probing from render/frame
   preparation translation units.

Expected result:

- Frame prep stops doing status-bar recomputation.
- Git cannot cause accidental UI stalls.
- Status-bar state becomes a service-owned cache rather than derived render-time work.

## Finding 9: Diff, Merge, And Compare Are Mostly Bounded But Need Large-Case Work

Relevant code paths:

- `src/compare/CompareModel.cpp`
- `src/compare/MergeModel.cpp`
- `src/workspace/WorkspaceShellCompareRender.cpp`
- `src/workspace/WorkspaceShellMergeRender.cpp`

The compare model has useful bounds around LCS and intraline matrices, which is the right policy.
The next risks are large-conflict merge grouping and per-frame marker preparation.

Potential bottlenecks:

- Merge grouping expands interacting changes with nested scans and can become quadratic on many
  adjacent or overlapping changes.
- Merge scrollbar marker inputs are rebuilt from conflicts each frame.
- Result-line display paths should be checked for full-result copying when conflict state changes.
- Compare and merge render paths should share editor row/layout caches where possible instead of
  building parallel string/render structures.

Rewrite plan:

1. Replace merge interaction grouping with a sweep-line interval grouping algorithm.
2. Cache merge scrollbar markers by conflict/model revision.
3. Add pathological merge perf fixtures with many tiny adjacent conflicts and many non-overlapping
   conflicts.
4. Share editor text layout/render caches for compare and merge rows where the row model is
   structurally compatible.

Expected result:

- Merge performance becomes predictable for large generated conflicts.
- Compare/merge rendering benefits from the same text backend improvements as the editor.

## Finding 10: Plugin Hover Can Block The UI Path

Relevant code paths:

- `src/workspace/WorkspaceShellHoverTargets.cpp`
- `src/workspace/WorkspaceShellHoverPopup.cpp`
- `src/plugin/*`

Plugin hover queries are made from the hover target path and currently build line strings before
querying the plugin host. Even if current plugins are cheap, the boundary allows arbitrary provider
work to happen in response to pointer movement or hover resolution.

Rewrite plan:

1. Debounce hover provider queries.
2. Cache hover results by provider generation, document generation, path, line, and column.
3. Make plugin hover providers budgeted or asynchronous if real plugins show non-trivial latency.
4. Avoid copying line text for hover queries when a string view or line ID is sufficient.

Expected result:

- Pointer movement and hover target calculation stay responsive even with plugin-provided hovers.
- Plugin cost becomes observable and bounded rather than hidden in shell interaction paths.

## Measurement And Tooling Gaps

The current perf infrastructure is valuable, but the next round of optimization needs better
attribution.

Required additions:

1. Phase-level allocation accounting in `PerfHarness`.
2. Frame-prep counters for settings application, status-bar refresh, render view-model builds, and
   layout recomputation.
3. Editor counters for full wrapped-row rebuilds, syntax checkpoint resets, encoding rescans,
   full-document line copies, and history slice sizes.
4. Renderer counters for text width cache hits/misses, texture cache hits/misses, evictions, glyph
   atlas hits/misses, rendered strings, and rendered glyphs.
5. Terminal counters for copied visible cells, scrollback trims, line-run cache hits, and run
   strings built.
6. Search/index counters for path copies, lowercased bytes, progress events, result snapshots, and
   file-index snapshot sizes.
7. A perf scenario inventory check that fails when baseline files exist for scenarios that are no
   longer registered.

The baseline directory currently includes outline-related baselines that did not appear in the
registered scenario search during this investigation. Either restore those scenarios or remove the
stale baselines so the perf matrix remains authoritative.

## Prioritized Rewrite Plan

### P0: Make Bottlenecks Measurable

Add phase allocation metrics and the counters listed above. This prevents optimizing setup cost
when the interaction cost is already healthy, and it makes regression review objective.

Acceptance target:

- Perf JSON reports phase wall time and phase allocations.
- Editor/render/terminal/search counters are available in perf runs.
- Scenario/baseline drift is detected automatically.

### P1: Remove Redundant Frame Work

Make settings and status bar updates event-driven. Build each editor pane render plan once per
frame and reuse it for content, overlays, and scrollbars.

Acceptance target:

- `ApplyEditorPreferencesToAllTabs()` is not called during steady frame prep.
- Each editor pane builds one final render view model per frame.
- Status bar refresh does not probe git, detect language, or rebuild stable strings per frame.
- `multi_tab_cycle`, sticky scroll, fold refresh, whitespace paint, and indent guide paint improve
  without changing editor semantics.

### P2: Rewrite Editor Edit Invalidation

Split revisions, add non-soft-wrap direct visual row mapping, defer or incrementalize encoding
refresh, make syntax invalidation range-aware, and remove full-document edit snapshots.

Acceptance target:

- Small edits near the top of a 50k-line file do not rebuild full wrapped-row layout.
- Multi-caret operations allocate by touched ranges, not file size or caret span distance.
- `editor_smart_indent_typing`, `editor_auto_close_typing`, `editor_fold_recompute`, and
  `editor_surround_multi_caret` become bounded and stable.

### P3: Replace Render Text Hot-Path Allocation

Introduce line-local layout caching and a glyph/cluster atlas for editor and terminal text. Make
whitespace and occurrence data row-indexed.

Acceptance target:

- Scrolling through unique code lines has high glyph/cache reuse.
- Whole-string texture churn no longer dominates editor or terminal paint.
- Text renderer cache stats are part of perf reports.

### P4: Rework Terminal, Search, And File Index Structures

Move terminal scrollback to a paged/ring store, make visible lines zero-copy, make file index
snapshots shared by generation, and make project search allocation-free for literal case-insensitive
scans.

Acceptance target:

- `terminal_scroll_long_output` allocation count drops by at least an order of magnitude.
- File finder remains responsive on a 100k-file fixture.
- Project search progress/result updates are coalesced and delta-based.

### P5: Harden Large Diff/Merge And Plugin Boundaries

Use sweep-line merge grouping, cache merge markers, add pathological merge scenarios, and debounce
or async plugin hover.

Acceptance target:

- Merge performance is predictable for many-conflict inputs.
- Plugin hover cannot block pointer movement or steady render paths.
- Compare/merge rendering uses shared text layout/render infrastructure where practical.

## What Not To Do

Avoid fixes that hide the same broad work behind another cache without changing invalidation
ownership. The current bottlenecks are mostly caused by doing too much work too often. A cache helps
only when the invalidation key is narrow, measurable, and tied to the actual state that changed.

Avoid preserving compatibility shims around the current frame-driven settings and edit-history
paths. These are internal boundaries and should be broken if that produces a smaller, faster, more
explicit system.

Avoid treating software-rendered text as a collection of arbitrary full strings forever. That model
is easy to implement but mismatched to code editing, where most scrolled lines are unique and most
glyphs are not.
