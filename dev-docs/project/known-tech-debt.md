# MicroIDE Known Tech Debt

Reviewed on 2026-04-23. Updated 2026-04-29 after comprehensive tech-debt cleanup slices.
Updated 2026-05-18 with rejected refactor experiment notes.
Updated 2026-05-19 with project-search throughput and perf-compare measurement fixes.
Updated 2026-05-19 with post-`e9a4764` perf-compare null-result investigation and the
two follow-up items it surfaced (merge scrollbar marker cache, hover visual-column map).
Updated 2026-05-20 with item #16 phase-1-through-3 progress (companion cap 51→45,
`WorkspaceTabStripChrome` adapter) and item #15 phase-1 progress
(`TextViewportUndoHistory` extraction, the first real ownership reduction since the
2026-05-18 file decomposition pass).
Updated 2026-05-20 with scoped async tree git-status refresh (status-only automatic
refreshes, first-paint tree badges, full refresh on Git sidebar open) and the new
large-file / compare / merge perf gates that close items 5 and 17.3.

This document records the meaningful debt that remains after commit `0aa44cb`
(`Fix shared diff/search paths and active editor state`).

Use this file for deferred work that is real, actionable, and still open.
Use `dev-docs/project/active-work.md` for current priorities.
The broader architectural review (from 2026-04-20) is archived at `dev-docs/archive/production-tech-debt-review.md`.

## Scope

This list focuses on debt that is:

- still present in the current tree
- likely to affect correctness, latency, or extensibility
- worth preserving as a future work queue

This list intentionally does not repeat issues that were already closed in
`0aa44cb`, including:

- merge using its own quadratic line-diff matrix
- merge conflict grouping staying quadratic in conflict count after the line-diff pass
- `ComputePopupMenuRect` recomputing popup item widths on every per-frame call (render,
  redraw planner, mouse hit-test, cursor manager, and submenu paths each used to walk
  every item × `MeasureWidth(label) + MeasureWidth(accel)` independently; now memoized
  by `(items.data(), items.size(), LSP readiness)` in a thread-local 6-entry LRU)
- `WorkspaceShellHoverTargets.cpp` walking `VisualColumnForTextColumn` three times per
  hovered row instead of building one `LineVisualColumnMap` (item 17.2)
- merge render rebuilding `BuildMergeScrollbarMarkers` every frame with no cache
  (`MergeTabState` now carries `model_revision` + cached marker list keyed on the
  track rect, mirroring the compare-side cache; item 17.1)
- per-mouse-motion plugin hover re-issuing identical `QueryHover` calls for every
  pixel that maps to the same `(path, line, text_column)` (now position-memoized via a
  thread-local last-query cache)
- per-frame `UpdateMouseCursor` re-running `CursorKindForPosition` when the inputs it
  reads (mouse position, drag target, prompt/menu visibility, workspace layout
  revision) are unchanged
- `FileIndex` snapshot copy on every search start (`FilePathSnapshot` and the search
  service now share `SharedPathList` = `std::shared_ptr<const vector<path>>`; the cache
  rebuilds into a new shared_ptr so consumers iterate the cache directly with zero
  copies)
- per-scope wake reason invisibility in `WorkspaceWakeController::HandleScheduledWake`
  (counters `workspace.wake_reason_plugin_reload`, `workspace.wake_reason_caret_blink`,
  and `workspace.wake_reason_none` now identify which path each scheduled wake took,
  so `idle_soak_30s` can attribute the residual ~50 prepares/iter to a specific source)
- merge result text always serializing with `\n`
- project search rescanning disk on every run instead of consuming an indexed snapshot
- project search allocating a lowercase copy of every candidate line in case-insensitive mode
- project search snapshotting the full cumulative result set back to shell state on every consume
- LSP `textDocument/didOpen` building the full JSON payload on the UI thread before queueing
- local `tools/perf-compare.py` single-scenario comparisons being biased by fixed side order
- `TextViewport::MaxVisualColumns()` always rescanning the entire buffer after ordinary edits
- FIFO text-render cache eviction in `SdlTtfTextBackend`
- split-editor actions and several open-or-navigate paths mutating the stale floating editor copy

## Closed Debt From Comprehensive Cleanup

The following previously tracked debts were closed on 2026-04-29 by
`comprehensive-tech-debt-cleanup`:

- item 1 (`WorkspaceShell` ownership bottleneck): closed
  - `WorkspaceShell.h` now satisfies the architectural size contract
  - legacy `WorkspaceActionContext` file names were removed from the tree
- item 2 (coordinator separation still superficial): closed for this phase
  - architectural lint now hard-fails key boundary regressions
- item 3 (active editor viewport ownership migration): closed
  - stale shell-level viewport alias paths were removed
- item 4 (render and hover shell reach): closed for this phase
  - render-path architectural constraints are enforced by lint
- item 7 (single-line shell text input model): closed
  - shared single-line editor and key-handler model is now shipped
- `WorkspaceLspClient` TSAN race (reported during sanitizer bring-up): closed
  - request/callback ownership synchronization was fixed and verified with TSAN runs in the sanitizer matrix

## 17. Post-`e9a4764` Throughput-Pass Follow-Ups

Status:
- Closed on 2026-05-19 except for the fixture work in 17.3. The two symmetric seams
  (merge scrollbar marker cache, hover-targets `LineVisualColumnMap`) landed in the
  same pass that closed item 5 and the menu / git-dispatch / wake-reason items above.
  Originally open after `e9a4764` ("perf: land throughput fixes and stabilize perf
  compare"). The three landed changes (merge-conflict grouping →
  linear pass, compare-surface `LineVisualColumnMap`, compare scrollbar-marker cache keyed
  by `model_revision` + track rect) are correct and unit-tested but the current perf
  fixtures don't exercise their worst cases, so the wall-time delta on `perf-runner-v1`
  is inside the 2σ stdev band the new perf-compare classifier dims. The follow-ups below
  finish the remaining symmetric work; new fixtures to actually surface the existing
  wins are tracked under item 6.

What was bench-invisible and why (recorded so we don't repeat the investigation):
- Merge grouping: the `merge_scroll_large_fixture` is a tail-only 1 MB diff producing
  ~2 `SideChange`s, so `O(N²)` vs `O(N)` on N=2 is identical at frame scale.
- `LineVisualColumnMap` in `WorkspaceShellCompareRender.cpp`: only fires inside the
  selection / caret branches; the burst scenarios scroll without holding a multi-row
  selection, so the per-row deduplication has nothing to deduplicate.
- Compare scrollbar marker cache: real but small — `BuildCompareScrollbarMarkers` is a
  cheap comparison + pointer bump per model row, so eliminating ~30 k iterations × 80
  frames × 10 iters saves single-digit µs/frame; the win sits in the noise band.

### 17.1 Merge Render Has The Same Per-Frame Scrollbar Marker Rebuild

Status:
- Closed on 2026-05-19. `MergeTabState` now carries `model_revision`,
  `scrollbar_marker_cache_valid`, `scrollbar_marker_cache_revision`,
  `scrollbar_marker_cache_track`, and `scrollbar_marker_cache`; `RefreshMergeTabDerivedState`
  bumps the revision and invalidates the cache; `DrawMergeScrollbarMarkers` consumes
  the cache, gated on revision and track-rect equality (mirrors the compare-side
  pattern landed in `e9a4764`).

Impact (kept here for context):
- Low to medium. Symmetric with the compare-side cache landed in `e9a4764`.
  `WorkspaceShellMergeRender.cpp:67` calls `BuildMergeScrollbarMarkers(track, total_rows,
  inputs)` every frame with no cache. For large fixtures the cost is small per frame but
  scales with model row count and is rebuilt unconditionally even when nothing changed.

Proposed shape:
- Mirror the compare-side pattern on `MergeTabState`:
  - Add `model_revision`, `scrollbar_marker_cache_valid`,
    `scrollbar_marker_cache_revision`, `scrollbar_marker_cache_track`,
    `scrollbar_marker_cache` fields.
  - Bump `model_revision` and invalidate the cache from the same merge-side
    refresh function that bumps the compare-side counterpart.
  - Gate the `BuildMergeScrollbarMarkers` call on (revision changed || track rect changed).

Relevant code:
- `src/workspace/WorkspaceShellMergeRender.cpp` (DrawMergeScrollbarMarkers call site)
- `src/workspace/WorkspaceTabState.h` (`CompareTabState` cache fields as the template)
- `src/workspace/WorkspaceShellCompare.cpp` (`RefreshCompareTabDerivedState` is the
  invalidation analog)

### 17.2 Hover Targets Still Walks `VisualColumnForTextColumn` Three Times Per Hover Row

Status:
- Closed on 2026-05-19. `PluginHoverTargetForLine` constructs a single
  `editor::TextLayout::LineVisualColumnMap` for the hovered line and resolves the
  end-of-line width / start visual / next code-point boundary against it.

Impact (kept here for context):
- Low. `src/workspace/WorkspaceShellHoverTargets.cpp:100,122,125` still issues three
  separate prefix walks of the same hovered line: end-of-line visual width, the
  text-column-under-cursor, and the next code-point boundary. The `LineVisualColumnMap`
  helper that landed in `e9a4764` is the right shape for this caller — three
  `O(line_length)` walks collapse into one build + three `O(log n)` queries.

Proposed shape:
- Construct one `editor::TextLayout::LineVisualColumnMap` for the hovered line at the
  top of the per-row block, and replace the three `VisualColumnForTextColumn(line_text,
  ...)` calls with `map.LineVisualWidth()` and `map.VisualColumnFor(...)`.

Relevant code:
- `src/workspace/WorkspaceShellHoverTargets.cpp:100`
- `src/workspace/WorkspaceShellHoverTargets.cpp:122`
- `src/workspace/WorkspaceShellHoverTargets.cpp:125`
- `src/editor/TextLayout.cpp` (helper already shipped)

### 17.3 Fixtures That Would Surface The Existing Wins

Status:
- Closed on 2026-05-20. `large_file_open_first_paint`,
  `merge_scroll_interleaved_hunks`, and `compare_scroll_selection` now run by default
  with committed local baselines, covering first paint, interleaved merge hunks, and
  multi-row compare selection during sustained scroll.

These don't change product code; they only make the gate sensitive enough to credit
the asymptotic work already in tree. Listed so we don't quietly revert any of the
landed changes for being "bench-invisible" before a representative fixture exists.

- A merge fixture that produces dozens-to-hundreds of interleaved hunks (drives item 1
  in `e9a4764`'s grouping rewrite).
- A compare-scroll scenario that holds a multi-row selection across the scroll burst
  (drives the `LineVisualColumnMap` selection branch).
- A merge-scroll scenario after 17.1 lands, to credit the symmetric cache.

## 5. Search and Index Integration — Event-Driven File Watch

Status:
- Mostly resolved on 2026-05-19 by `deferred-work-and-throughput-pass` plus the
  `perf/project-search-lower-snapshot` follow-up.

What was closed:
- `FileIndexWatcher` platform abstraction ships Linux `inotify`, macOS `FSEvents`, Windows
  `ReadDirectoryChangesW`, and a poll-fallback backend.
- `PatternCache` with LRU eviction eliminates repeated PCRE2 compile/JIT on repeated searches.
- `ProjectBackgroundExecutor` isolates per-project git dispatch from the main thread.
- `BackgroundTaskCounter` tracks in-flight background work for adaptive idle rendering.
- PCRE2 JIT is now compiled into the search engine; interpreted fallback emits a one-time log.
- `ProjectSearchService` wires `BackgroundTaskCounter` so the event loop stays awake during search.
- Workspace project-state wiring now starts/stops the file-index watcher with project lifecycle,
  and file-index updates invalidate/refresh dependent file-finder and search state.
- File finder reads from `FileIndex` snapshots instead of rescanning the tree on each refresh.
- Project search now starts from `FileIndex::SnapshotPathsWithVersion(...)`, reuses a single
  lowercase line buffer in case-insensitive literal mode, and publishes incremental result deltas
  instead of snapshotting the entire cumulative result set back to the shell on every consume.

What is still open:
- Blame and log dispatch are already off the UI thread.

What was closed on 2026-05-20:
- `DirectoryTree::RefreshGitStatuses()` no longer runs during project set-root, and
  `WorkspaceSidebarCoordinator::ShowGit()` no longer calls it synchronously. The tree
  status map is now built from the existing async Git sidebar working-tree snapshot and
  applied through `DirectoryTree::ApplyGitStatuses()`. After the first paint on project
  open, a scoped async refresh materializes tree badges without blocking startup or
  collecting outgoing-branch files. Automatic status-only refreshes still skip tree
  badge materialization until that first-paint hook runs.

Investigation note (2026-05-19 perf-compare diagnostic):
- Clean `e9a4764` vs `27943f9` baseline: no regression beyond noise.
- `e9a4764` + the unconditional `RefreshGitSidebar` on project set-root: +500k–1.4M
  allocations across most scenarios that open a project, +27–30% wall on cold-startup
  fixtures.
- ITER=10 ablation isolating the change: reverting just that one call site dropped
  allocations to within +0.01% of baseline across the full focused scenario set
  (`idle_soak_30s`, `editor_auto_close_typing`, `menu_hover_switch`).
- Root cause: each `RequestGitSidebarRefresh` posts a worker task that spawns 4 git
  subprocesses (`CollectGitWorkingTreeEntries`, `ResolveGitOutgoingBase`,
  `ResolveGitBranchLabel`, `CollectGitBranchOutgoingFiles`). Subprocess setup
  (env-table copy, pipe FDs, stdio buffers, output parsing) costs ~100k+ short-lived
  allocations per spawn. The allocations themselves are freed promptly (RSS does not
  regress), but the churn is pure overhead when the user has not opened the Git
  sidebar.
- Lesson: "async" does not mean "free". A background task that allocates ~480k strings
  on every project open still bills against the process allocator counter and the
  cold-startup wall budget. Future async migrations should be gated on user-visible
  demand or measured to be allocation-cheap before being made unconditional.

Recommended follow-up:
- Keep future tree-badge work scoped to user-visible demand or first-paint hooks. If
  tree badges become visible outside the Git sidebar, measure the async snapshot cost
  before making it unconditional on project open.

## 6. Large-File and Performance Validation Still Needs Measurement, Not Assumptions

Impact:
- Medium
- This is process debt with real product consequences

What is still open:
- The recent fixes remove known hotspots, but new large-file behaviors still need empirical
  validation before they are treated as safe.
- The syntax-highlight jump problem in particular should be measured before and after any future
  checkpoint design.
- Search, merge, blame, and redraw changes should continue to be validated with the startup and
  runtime profiling docs rather than by intuition.
- The local comparison helper `tools/perf-compare.py` now runs each scenario in both side orders
  and merges the raw iteration streams before recomputing p50/p95/max, which removes the
  single-scenario fixed-order bias that previously produced false fold/whitespace signals. It is
  still advisory only; `perf-runner-v1` remains the authoritative gate.
- LTO is enabled for perf/release builds and currently helps recover some cross-translation-unit
  optimization loss from editor extractions, but that is not evidence the extraction is free.
  Any residual sticky-scroll/render-path regression should be profiled directly, then either fixed
  or explicitly accepted with data.
- The harness now includes a dedicated large-file open-to-first-paint gate and representative
  compare / merge sustained-scroll gates for interleaved hunks and multi-row compare selection,
  but it is still narrower than a full interaction matrix. Large-fixture hunk navigation and
  mixed edit/scroll traces remain open.

References:
- `dev-docs/performance/startup-tracing.md`
- `dev-docs/performance/runtime-profiling.md`
- `dev-docs/performance/performance-findings.md`

Recommended follow-up:
- Add or extend focused benchmarks where repeated regressions are likely:
  - large-file cursor jump and initial paint
  - compare / merge hunk-navigation or mixed edit/scroll interaction on large fixtures
  - merge-model build for large, partially similar inputs
  - project search across large trees with smart-case and regex modes
  - syntax cache invalidation after plugin reload

## 7. Single-Line Shell Text Input Model

Status:
- Addressed on 2026-04-29 by introducing shared `SingleLineEditor` and `SingleLineKeyHandler` for migrated single-line surfaces

Impact:
- Closed in this change.

What remains:
- No further migration is required for task closure.

Relevant code:
- `src/editor/SingleLineEditor.h`
- `src/editor/SingleLineKeyHandler.h`
- `tests/SingleLineEditorTests.cpp`

## 8. `WorkspaceReviewComments` Does Linear Scans Per Frame When Review Comments Are Active

Status:
- Addressed on 2026-04-23 by indexing review comments and threads per URI and line inside
  `WorkspaceReviewComments`

Impact:
- High when the review-comments feature is populated; zero cost when there are no comments

Current state:
- `GetThreads(uri)` and `GetComments(uri, line_index)` now read from indexed URI/line buckets
- The render path uses `HasThreads(uri, line)` and `HasComments(uri, line)` for marker checks
- Add, remove, and clear operations invalidate the affected indexes before the next lookup

What remains:
- No known follow-up for this item unless profiling shows review marker drawing itself is still hot

Relevant code:
- `src/workspace/WorkspaceReviewComments.cpp` — `GetThreads`, `GetComments`
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda

See also `dev-docs/performance/performance-findings.md` — Second Performance Pass, New finding 1.

## 9. `ComputeEditorPaneLayouts` Called Twice Per Render Frame

Status:
- Addressed on 2026-04-23 by computing editor pane layouts once in `RenderActiveWorkspaceSurface`

Impact:
- Medium; redundant geometry computation on every frame

Current state:
- `WorkspaceShellRenderFrame.cpp` computes the pane layout once near the top of the active
  workspace-surface render path and reuses it for the main editor render pass and scrollbar pass

What remains:
- No known follow-up for this item unless profiling shows pane layout computation is still hot

Relevant code:
- `src/workspace/WorkspaceShellRenderFrame.cpp`

See also `dev-docs/performance/performance-findings.md` — Second Performance Pass, New finding 2.

## 10. Terminal Cursor State Acquired Under Three Separate Mutex Locks Per Frame

Status:
- Addressed on 2026-04-23 by adding `TerminalSession::CursorSnapshot()`

Impact:
- Medium; three mutex round-trips on the render thread every frame when terminal is visible

Current state:
- Terminal render, caret invalidation, and pending-input capture use `CursorSnapshot()` to read
  row, column, and visibility under one mutex acquisition

What remains:
- The legacy scalar accessors still exist for tests and non-hot callers; remove them only if a
  later cleanup proves they are unused

Relevant code:
- `src/terminal/TerminalSession.h` — `TerminalCursorSnapshot`, `CursorSnapshot()`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — terminal cursor render path

See also `dev-docs/performance/performance-findings.md` — Second Performance Pass, New finding 3.

## 11. `std::find` on `marked_lines` Vector in Review-Comment Marker Rendering

Status:
- Addressed on 2026-04-23 by removing `marked_lines` from render marker drawing

Impact:
- Medium; O(visible_lines × marked_lines) per frame when review markers are present

Current state:
- `draw_review_comment_markers` performs direct indexed thread/comment checks per visible line
- There is no per-frame marked-line vector allocation and no per-line `std::find`

What remains:
- No known follow-up for this item unless review-marker rendering becomes a measured hotspot again

Relevant code:
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda

See also `dev-docs/performance/performance-findings.md` — Second Performance Pass, New finding 4.

## 15. `TextViewport.cpp` Ownership Concentration

Status:
- **UndoHistory seam landed on 2026-05-20** (commit `715b66b`,
  `refactor: extract TextViewportUndoHistory off TextViewport`). The first real
  ownership reduction since the 2026-05-18 file decomposition pass.
  `src/editor/TextViewportUndoHistory.{h,cpp}` now owns `undo_stack_` /
  `redo_stack_` (moved out of `DocumentState`), the per-viewport `group_stack_`
  (was `undo_group_stack_` on the viewport), the `HistoryEntry` / `ViewState` /
  `SecondaryCaret` type definitions, and the static helpers `ApplyEntryToLines`,
  `BuildAppliedEdit`, `BuildEntryForDocumentChange`, plus the private
  `TryMergeGroupEntry` / `ReconstructFallbackLines` merge math. `TextViewport`
  keeps `ApplyHistoryEntry` / `BuildRangeHistoryEntry` / `BuildLineHistoryEntry`
  (they touch viewport-private caches and CaptureViewState) and now forwards
  `Push*` / `Begin*` / `Finish*` through `undo_history_`. `TextViewport.cpp`
  shrank 1,788 → ~1,500 lines; `TextViewport.h` dropped the four nested struct
  bodies plus five private / static method declarations. New trivial value
  header `editor/EditTypes.h` carries `TextPosition` / `SelectionRange` /
  `AppliedEdit` so the new TU can depend on them without a circular include of
  `TextViewport.h`. Perf gate (`tools/perf-compare.py main`, the seven scenarios
  cited in the rejected `TextDocumentModel` section below): no metric regressed
  beyond the 2σ band; most scenarios moved slightly negative or improved.
- Substantially addressed on 2026-05-18 across five earlier extractions, all
  keeping methods as members of the same `TextViewport` class (no header / API
  change, no friending):
  - **Language-pair behavior** (auto-close, surround, skip-over-close, dedent-on-close,
    brace-split, smart-indent newline, multi-caret pair-insert, plus `AutoIndentForNewline` /
    `IndentUnit` / `InInsertionSuppressedScope`) →
    `src/editor/TextViewportLanguageBehavior.cpp`. Shared file-scope helpers promoted to
    `src/editor/TextViewportInternal.h` (detail namespace, internal header).
  - **Save normalization + file I/O** (`OpenFile`, `Save`, `LoadContent`, `SetPath`, `SetDirty`,
    `LineEndingLabel`, `EncodingLabel`, `RefreshEncoding`, both `DetectEncoding` overloads,
    plus `TrimTrailingWhitespaceInPlace` / `EnsureSingleFinalNewlineInPlace`) →
    `src/editor/TextViewportFileIO.cpp`.
  - **Multi-caret apply pipeline** (`ApplyMultiCaretInsert`, `ApplyMultiCaretBackspace`,
    `ApplyMultiCaretDeleteForward`) → `src/editor/TextViewportMultiCaret.cpp`. Calls into
    history helpers (`BuildRangeHistoryEntry`, `ApplyHistoryEntry`,
    `BuildHistoryEntryForDocumentChange`, `BuildAppliedEditForHistoryEntry`,
    `PushHistoryEntry`) that remain in `TextViewport.cpp`.
  - **Highlight cache** (`HighlightedLineTokens`, `HighlightedLineTokensIfCached`,
    `EnsureInitialHighlightState`, `EnsureHighlightCaches`, `EnsureHighlightCheckpoint`,
    `HighlightStateBeforeLine`, plus `IsCachedHighlightState` and `kHighlightCacheLimit`)
    → `src/editor/TextViewportHighlightCache.cpp`. `kHighlightCheckpointInterval` was
    promoted to `TextViewportInternal.h` because the invalidation policy in
    `InvalidateDerivedCaches` (still in `TextViewport.cpp`) needs to stay in lockstep with
    the checkpoint chain.
  - **Viewport view-state + cursor / scroll movement** (`SetViewportSize`, scroll setters,
    wrap / fold toggles, cursor movement, `EnsureCursorVisible`, wrapped-row cursor mapping,
    caret advance helpers, and `EnsureDocument`) → `src/editor/TextViewportViewState.cpp`.
- Result: `TextViewport.cpp` is now ~763 lines, down from ~3,553, with no public API change.
  The extracted editor TUs are focused by ownership: `TextViewportViewState.cpp` owns movement /
  scrolling state, `TextViewportEditEngine.cpp` owns insert/delete/range/undo application,
  `TextViewportMultiCaret.cpp` owns aggregate multi-caret application, and `TextLayoutCache.cpp`
  owns wrapped rows, visible-line cache, and max-column caching.

Remaining:
- Low. The host TU no longer owns cursor/scrolling state, edit application, undo grouping, or
  visible-line / wrapped-row layout caches. It still owns the shared `TextViewport` state and the
  invalidation contract between editing, highlighting, folding, and layout. That remaining coupling
  is intentional until a deeper document-buffer boundary is justified by measurements.

Why this item stays open at "low":
- File decomposition has done most of what it can without changing the public viewport API. The
  remaining `TextViewport.cpp` is now a coherent coordinator for selection, cache invalidation, and
  document metadata, not a catch-all.
- The next `TextViewport` work should reduce ownership, not merely line count. Avoid adding more
  sibling `TextViewport*.cpp` files unless they are a stepping stone toward moving state and
  behavior into smaller tested objects.

Recommended ownership seams for the next pass:
- `TextDocumentModel` / `DocumentBuffer`
  - line storage
  - line endings
  - dirty state
  - revision number
  - file-path or save metadata where appropriate
  - **Rejected — see "Rejected experiment" section below. Do not retry in
    the same shape.**
- `EditEngine`
  - range edits
  - multi-caret edit normalization
  - auto-pair edit transforms
  - edit grouping boundaries
  - range validation
  - Done as a focused TU split on 2026-05-20. The edit engine remains `TextViewport` member
    functions to avoid back-pointers or shared ownership in the per-keystroke path.
- `UndoHistory`
  - undo/redo stacks
  - grouping
  - caret/selection restore
  - revision integration
  - **Done on 2026-05-20 (commit `715b66b`). See Status above.**
- `TextLayoutCache`
  - soft-wrap rows
  - visible-line cache
  - fold-aware mapping
  - layout invalidation
  - Done on 2026-05-20 as `src/editor/TextLayoutCache.{h,cpp}`.

### Rejected experiment: `TextDocumentModel` ownership extraction

Status:
- Rejected on 2026-05-18 after benchmark-gate comparison against `main`.

What was attempted:
- The branch `refactor/text-document-model` extracted document-owned state from `TextViewport`
  into a `TextDocumentModel`.
- The model API encapsulated line storage, dirty state, revision counters, newline metadata, and
  mutation helpers.
- The goal was cleaner ownership boundaries and easier future extraction of edit/history concerns.

Outcome:
- Behavior tests passed in the focused editor suite.
- The performance gate failed with regressions in hot editor/render scenarios:
  - `editor_fold_viewport_refresh`: around +28–30% wall time
  - `editor_sticky_scroll_scroll`: around +21–23% wall time
  - `editor_indent_guides_paint`: around +20–21% wall time
  - `editor_render_whitespace_paint`: around +15–16% wall time
  - `editor_shaping_multi_caret`: up to around +15–22% wall time
  - `editor_auto_close_typing`: around +5–6% wall time
  - `typing_large_file`: around +5–7% p95/max wall time
- Broad allocation increases also appeared across typing, scrolling, idle, startup, terminal, and
  menu scenarios.
- The branch was abandoned rather than merged.

Lesson:
- `TextViewport` still needs decomposition, but not through an abstraction that degrades hot-path
  locality, increases hidden allocation/copy risk, or broadens cache invalidation.
- In this codebase, architectural cleanup is not acceptable if it materially hurts latency or
  render-path throughput.
- This was a successful benchmark gate: perf harness checks prevented a bad abstraction from
  landing on `main`.

Future attempts should prefer:
- tiny cold-path extractions first
- pure helpers operating on existing data without ownership changes
- no shared ownership for hot editor state unless proven free
- no full-buffer copies in render/layout/scroll/typing paths
- benchmark checks after each small change
- profiling before introducing API boundaries inside hot loops

Do not reintroduce `TextDocumentModel` in the same shape without first proving that line access,
mutation, revision updates, and cache invalidation are allocation-free and performance-neutral in
the editor benchmarks.

## 16. `WorkspaceShell*.cpp` Companion Sprawl Keeps Behavior In The Shell Namespace

Status:
- Three slices landed on 2026-05-20 that took the companion cap 51 → 45 and
  actually moved tab-strip / panel-tab behavior off `WorkspaceShell`:
  - Commit `e07e073` — collapsed four single-delegation companions
    (`WorkspaceShellInput.cpp`, `WorkspaceShellBlame.cpp`,
    `WorkspaceShellTerminalService.cpp`, `WorkspaceShellCommandPrompt.cpp`)
    into the bootstrapper / shell core. Cap 51 → 46.
  - Commit `06ef475` — folded `WorkspaceShellChrome.cpp` (the 15 tab-strip /
    overlay-rect / status-bar wrappers) into `WorkspaceShellPresentation.cpp`
    since they share the same `ProjectLabelForRoot` / `ProjectTabDisplayTitle`
    / `DefaultProjectBaseColor` presentation helpers. Cap 46 → 45.
  - Commit `b31b026` — extracted `WorkspaceTabStripChrome` (non-shell-named
    TU, doesn't count against the cap) holding refs to `WorkspaceContext`,
    `TabStripService`, `LayoutModeService`, `WorkspaceOutputChannels` plus an
    `Operations` struct for the shell-defined presentation / lifecycle hooks.
    The 12 tab-strip + bottom-panel-tab method bodies + the `ClearTabDrag`
    one-liner + the dead `static BuildVisibleStripTabs` declaration all left
    `WorkspaceShell`'s symbol surface; coordinator factories
    (`WorkspaceTabMouseCoordinator`), intra-shell call sites
    (`WorkspaceShellRenderChrome`, `WorkspaceShellCursor`,
    `WorkspaceShellRedraw`, `WorkspaceShellProjects`, `WorkspaceShellCompareMerge`,
    `WorkspaceShellPresentation`), the persistence and tab coordinators, and the
    test access inc files all now bind `tab_strip_chrome_` directly.

What is still open: `ComputeOverlayRect` and `RefreshStatusBar` remain on
`WorkspaceShell`. `ComputeOverlayRect` is a one-line wrapper around
`ComputeOverlaySurfaceRect` with 14 call sites — replaceable with a mechanical
rename pass. `RefreshStatusBar` is blocked from inlining at its single render-TU
call site by `CheckRenderSurfaceStateAccess` (no direct
`context_.current_project_state` access in render units); it would have to land
on a non-render service before the shell can drop it.

Impact:
- Low. The architectural-lint cap on `WorkspaceShell.h` (≤ 400 lines) and
  `WorkspaceShell.cpp` (≤ 600 lines) is satisfied; the companion cap is now 45
  (down from 51) and `CheckWorkspaceShellCompanionTuCount` enforces the new
  ratchet. `WorkspaceShellMembers.inc` is meaningfully smaller after phase 3.

Audit of the four originally-named candidates (2026-05-18):
- `WorkspaceShellOutput.cpp` (~56 lines, 4 methods): too small to migrate productively. Two
  methods are pure delegations to `output_channels_`; the other two mutate
  `current_project_state.panel.output.*` — inherent shell-state changes, not service-extractable
  behavior.
- `WorkspaceShellBlame.cpp` was a real service candidate (geometry, formatting, per-line lookup).
  This follow-up has now landed: `EditorBlameOverlayService` owns blame-overlay state, line lookup,
  hit-testing, and overlay construction for editor / compare surfaces. The shell keeps
  `GitBlameService` ownership and narrow invalidation / clear entry points.
- `WorkspaceShellAssist.cpp` was the third real service candidate. This follow-up has now landed:
  `AssistService` owns completion, snippet-session edits, code-action overlays, go-to-definition,
  and find-references coordination, and the old shell-specific assist facade has been removed.
  Follow-up remains: `AssistService::Operations` is a transitional seam. Avoid growing it into a
  general shell callback bag; prefer smaller explicit ports (`ActiveEditorPort`, `LspAssistPort`,
  `OverlayPort`, `CommandExecutionPort`, `FileOpenPort`, `MergeTrackingPort`, `CompareSyncPort`)
  when the next assist refactor is scoped.
- `WorkspaceShellChrome.cpp` was the second real service candidate. This follow-up is now further
  along: `TabStripService` owns editor/project/bottom-panel tab-strip layout state, overflow
  controls, bottom-panel tab models, and the geometry queries now used directly by render,
  cursor, mouse-coordinator, and test-access paths. The shell keeps only the bottom-panel
  activation/close side effects that still mutate project state and terminal/output ownership,
  plus a smaller set of project/editor-tab wrappers.

What was actually done in the low pass (2026-05-18):
- Ratchet-only architectural lints added at `tests/ArchitectureInvariantsTests.cpp`:
  - `CheckShellFileSize(WorkspaceShellMembers.inc)` caps the file at 1,516 lines (current).
  - `CheckWorkspaceShellCompanionTuCount` caps the count of `WorkspaceShell*.cpp` translation
    units at 51 (current).
  Both are hard-fail. These caps are regression guardrails, not the desired end state. Lower the
  cap when a migration shrinks either number; never raise.

Recommended follow-ups (deferred, each a separate medium-sized change):
- Replace `ComputeOverlayRect(x)` with `ComputeOverlaySurfaceRect(x)` at the
  ~14 call sites and drop the shell wrapper. Mechanical.
- Move `RefreshStatusBar` body onto a non-render service so the shell can drop
  it (the current blocker is `CheckRenderSurfaceStateAccess`, not the wrapper
  itself).
- Do not add new `WorkspaceShell*.cpp` files for new behavior — the cap now
  hard-fails this.

## Open Follow-Ups After The 2026-04-29 Cleanup

The cleanup change closed items 1–4 and 7 above and shipped the durable contracts in
`openspec/specs/workspace-architecture/spec.md`,
`openspec/specs/persisted-state-format/spec.md`, and
`openspec/specs/shared-edit-primitives/spec.md`. These follow-ups are still worth tracking and
are good candidates for the next openspec tech-debt pass:

1. `WorkspaceShellTestAccess.h` trim follow-up:
   - Closed in the comprehensive tech-debt and perf-harness pass.
   - The top-level header is now a small scoped aggregator with a hard architectural size gate,
     and category-(a) wrappers were migrated to direct shell APIs / event helpers.
   - Keep the remaining scoped methods focused on genuinely test-only seams.
2. The `WorkspaceShell*.cpp` companion files (~70 translation units defined against
   `WorkspaceShellMembers.inc`) keep behavior on the shell namespace even though the header was
   slimmed. Any new behavior should land on a service, not a new `WorkspaceShell*.cpp` companion.
3. Legacy persistence importer follow-up:
   - Closed in `codebase-cleanup-perf-and-debt`; `WorkspacePersistenceLegacyFormat.*` was deleted
     and persistence now stays on structured records only.
4. Architectural-lint coverage gap:
   - Closed in this change: discovered render-unit scanning is active, plugin/coordinator size
     checks are hard-fail, and the shell test-access header now has an explicit cap check.
5. Oversized coordinator translation units:
   - Closed in this change: coordinator units were decomposed and the coordinator TU-size rule is
     hard-fail.
6. Project-content and indexing architecture (item 5) now has an event-driven watcher layer and
   background executor, but workspace wiring (file-finder, search, git dispatch) is deferred to the
   next pass. Revisit when the `deferred-work-and-throughput-pass` wiring tasks (2.2–2.5, 3.2–3.6)
   are scoped into an openspec change.

## 8–12 Status Update (2026-05-01)

The debt items tracked as 8–12 in this document are now resolved or explicitly narrowed:

- item 8 (review-comment linear scans): resolved; indexed lookup path remains in place
- item 9 (double layout computation per frame): resolved; layout is prepared once and reused
- item 10 (terminal cursor multi-lock reads): resolved; cursor snapshots are single-lock
- item 11 (`std::find` over `marked_lines` in render path): resolved; vector scan removed
- item 12 (sanitizer/fuzz triage tracking): reduced to environment/process follow-up only

Remaining debt focus stays on items 5 and 6 unless new profiling demonstrates regressions.

## 13. Do Not Revisit The Editor Glyph Atlas Without GPU Renderer + ≥ 10 % Texture-Cache Miss Rate

Source: `dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md` "Rejected experiment: ASCII glyph atlas".

Impact:
- Saves engineering time on a previously-attempted dead end.

What was tried (2026-05-15):
- One alpha-only `SDL_Texture` ASCII atlas keyed by `(font face, font size)`,
  per-glyph src rects, `SDL_RenderGeometry` fast path in `SdlTtfTextBackend`,
  per-vertex color, three perf counters, opt-in flag.
- End-to-end working: counters fire, evictions stay at 0, texture-cache misses
  drop to ~1 per iteration.

Measured outcome:
- Wall-time regression on every editor paint scenario on the software renderer:
  `editor_render_whitespace_paint` +81 %, `editor_sticky_scroll_scroll` +83 %,
  `editor_indent_guides_paint` +48 %.
- Reverted in full. `MICROIDE_RENDER_GLYPH_ATLAS` no longer exists.

Why we were wrong:
- The composite texture cache is at > 99 % hit rate on every paint scenario,
  so cache-miss thrash is **not** the bottleneck the design targeted.
- `DrawString` is called at the **run** level, not the cell level — the
  composite cache already operates on whole same-color runs and is served by
  one `SDL_RenderTexture` call per cached string.
- `SDL_RenderGeometry` in the software renderer is not a free batched
  primitive: per-vertex color modulation forces per-pixel attribute
  interpolation, so the atlas's "one call per run" submits strictly more
  per-pixel rasterization work than the composite blit it replaces.

Do not propose another glyph-atlas variant for the editor text path unless
**all three** of the following preconditions hold (lifted verbatim from the
round-4 rejection section so reviewers can hold any future proposal to them):

1. MicroIDE is rendering through a **GPU backend, not software**. The
   software path remains the perf-gated path; per-pixel work scales the same
   way for `SDL_RenderTexture` and `SDL_RenderGeometry` there.
2. A **measured fixture exists where `render.text_texture_cache_misses /
   cells_visited` exceeds ~10 % in steady state.** Today's number is < 1 %.
3. A trace shows `BuildAsciiCompositeSurface` /
   `SDL_CreateTextureFromSurface` as a **top-3 hotspot** on
   `perf-runner-v1`. Today they are far below the per-pixel blit cost.

If those preconditions ever hold, the right shape is *"reuse atlas data to
build a composite string texture on cache miss"* — preserving the
one-`SDL_RenderTexture`-per-cached-string draw shape the software renderer
is happy with — not the per-quad geometry approach attempted here.

The OpenSpec record of the experiment is kept at
`openspec/changes/text-renderer-glyph-atlas/` for archaeological purposes
only. It is rejected; **do not apply**.

## 14. Split `document_->layout_revision` Into Tiered Revisions

Status:
- **Closed on 2026-05-15** by openspec change `split-layout-revision-tiers`
  (commit a0fdc58). `TextViewport::DocumentState` now exposes four tiered
  revisions (`content_revision`, `syntax_revision`, `layout_shape_revision`,
  `presentation_revision`) plus an `InvalidationReason` enum on the rewritten
  `InvalidateDerivedCaches(reason, start_line)` entry point; every derived
  cache (wrapped-row layouts, highlight, bracket-match, indent-guides,
  occurrence seed/scan, status-bar language, folding fingerprint) keys on the
  minimum tier set it actually depends on. The architectural-lint test now
  hard-fails on reintroduction of a combined `layout_revision` member on
  `DocumentState`. Per-tier perf counters
  (`editor.{content,syntax,layout_shape,presentation}_revision_bumps`) are in
  place; the `editor_scroll_only_no_content_bump` perf scenario and updated
  baselines are deferred follow-ups tracked in
  `openspec/changes/split-layout-revision-tiers/tasks.md`.

Source: `dev-docs/performance/investigations/performance-bottleneck-deep-dive-2.md` Finding 16,
`dev-docs/performance/investigations/performance-bottleneck-deep-dive-3.md` partial,
`dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md` Finding 4 (partial).

Impact:
- Medium-to-high. Every edit currently bumps a single `document_->layout_revision`,
  invalidating four logically independent caches in `TextViewport`:
  visible-line layout, syntax-highlight tokens + checkpoints, fold model, and presentation
  (width cache key set, decoration spans). A one-character insertion past the visible region
  still cascades into derived-cache wipes across the suffix of the document.
- The round-4 lazy-invalidation cursors (`line_highlight_states_valid_through_`,
  `highlight_checkpoints_valid_through_`) made the reset O(1), but **readers still recompute**
  unnecessarily because scrolls and non-content edits still bump the same revision.

Proposed shape (round-2 #16 / round-3 cascade):

- Split the single revision into four tiers on `TextViewport::Document`:
  - `content_revision` — bumped by edits to `lines`.
  - `syntax_revision` — bumped by language/theme/contract change.
  - `layout_shape_revision` — bumped by soft-wrap toggle, fold collapse, tab size, visible-columns.
  - `presentation_revision` — bumped by decoration / overlay-only changes (no buffer mutation).
- Re-key each cache on the **minimum set** of revisions it actually depends on:
  - `wrapped_row_layouts_` → `layout_shape_revision` (the round-4 fix already proves the value of dropping the fold-rev dependency).
  - `visible_line_cache_` → `content_revision`, `presentation_revision`.
  - `highlight_cache_` / `line_highlight_states_` / `highlight_checkpoints_` → `content_revision` + `syntax_revision`.
  - Width / texture caches in `TextRenderer` → `syntax_revision` (color theme) + font-id; do not invalidate on content edits.
- Make `InvalidateDerivedCaches(start_line)` route to the tier that actually changed
  instead of bumping a global revision.

Reproduction / measurement:

- Baseline today: `editor_sticky_scroll_scroll` p50 ~1.08 s (3 iters), `editor_indent_guides_paint` ~0.64 s, `editor_render_whitespace_paint` ~0.80 s.
- Counters to watch: `editor.invalidate_derived_caches_lines`,
  `editor.ensure_wrapped_row_layouts_rebuilds`, `editor.highlight_cache_forced_misses`.
- Add a scroll-only fixture asserting that scrolling does not bump `content_revision`.

Notes:

- This is the documented "honorable mention" alongside the font-atlas work
  (`dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md`). Surface is wide — touches every
  cache invalidation site across `TextViewport`, `EditorViewRenderer`, and the render
  view-model builder — so it should be scoped as its own openspec change rather than
  bundled with smaller optimization passes.
- Expected impact: 10–30 % wall-time reduction across many editor scenarios; unlocks
  additional small wins (width-cache stability across edits, per-tier counters).

## 12. 2026-04-29 Sanitizer/Fuzz Triage Snapshot

Status:
- In progress for this change; current non-blocking findings below are triaged with reproduction
  notes and severity.

### 12.1 TSAN Linux Prerequisite

Impact:
- Medium process risk (false negatives if skipped)

Reproduction:
- Run TSAN tests without setting Linux mmap randomization to the expected value.
- Command sequence:
  - `sudo sysctl vm.mmap_rnd_bits=28`
  - `cmake --preset microide-tsan`
  - `cmake --build build/microide-tsan -j8`
  - `ctest --test-dir build/microide-tsan --output-on-failure`

Notes:
- This is an environment prerequisite, not an app bug.
- Documented in `dev-docs/performance/runtime-profiling.md`, `guidelines/testing.md`, `AGENTS.md`, and
  `CLAUDE.md`.

### 12.2 UBSAN Intermittent FileWatcher Assertion Under Heavy Mixed Runs

Impact:
- Low to medium (intermittent in stressy mixed runs, not consistently reproducible in focused reruns)

Reproduction:
- Run broader sanitizer slices in quick succession; one run observed a transient FileWatcher
  assertion failure.
- Focused reruns for the affected area passed.

Notes:
- Keep as watchlist until it reproduces deterministically with a minimized command.
- If it reproduces again, capture exact command and stack and promote to a dedicated debt item.

### 12.3 Fuzz Harness Results (PR-style Short Runs)

Impact:
- No memory-safety findings observed in current short runs.

Reproduction:
- `cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`
- `cmake --build build/microide-fuzz -j8`
- `./build/microide-fuzz/microide/PersistedRecordReaderFuzz -max_total_time=10 tests/fuzz/corpora/PersistedRecordReaderFuzz`
- `./build/microide-fuzz/microide/LegacyImporterFuzz -max_total_time=10 tests/fuzz/corpora/LegacyImporterFuzz`
- `./build/microide-fuzz/microide/SearchRegexFuzz -max_total_time=10 tests/fuzz/corpora/SearchRegexFuzz`
- `./build/microide-fuzz/microide/GitBlameParserFuzz -max_total_time=10 tests/fuzz/corpora/GitBlameParserFuzz`

Notes:
- Initial clang/fuzz build surfaced integration defects (sized-delete portability in tests and
  missing object linkage in `LegacyImporterFuzz`), both fixed in-tree.
- No additional deferred fuzz finding is open from this triage pass.

## 18. 2026-06-11 Deep Correctness / Tech-Debt Audit Pass

A fan-out correctness/tech-debt audit across editor, platform, project, plugin, workspace,
and compare/persistence subsystems. The findings below are the substantiated ones; each was
verified against source before being recorded.

### Fixed in this pass (with regression coverage)

- **Persisted-state OOM on corrupt input**: `DecodeSplitNode` and the generic
  `PrimitiveReader::ReadVector` reserved an attacker-controlled `count` (up to 2^32-1) before
  reading any element, so a corrupt session file could force a multi-GB allocation. Both now
  clamp the reservation to the remaining input. `src/workspace/WorkspacePersistenceBinaryInternal.h`,
  `src/persistence/PersistedRecord.h`; test `PersistedStateRecord/RejectsAdversarialLengthWithoutOom`.
- **Keyboard-focus stranding ("dead input")**: confirming an external-URL prompt
  (`WorkspaceShellPrompts.cpp`, was `DismissPromptSurface(!opened)`), Escape from
  BufferReplace / ProjectSearch overlays (`WorkspaceKeyInputCoordinatorSurfaces.cpp`, hand-rolled
  `overlay.visible=false`), and renaming a file under an open commit picker
  (`WorkspacePathMutationCoordinatorTabs.cpp`) all left `surface.focus == Overlay` on a hidden
  surface. Centralized via `PrimarySurfaceFocus` / `HideOverlay` in `WorkspaceProjectState.h`;
  the Escape paths now route through the canonical `DismissOverlay`. New hard architectural
  invariant `CheckOverlayDismissalIsCentralized` forbids bare `overlay.visible = false` outside
  `WorkspaceShellOverlay.cpp` / `WorkspacePersistenceCoordinatorSession.cpp`.
- **Git branch-diff parser corrupted spaced paths / renames**: `CollectGitBranchOutgoingFiles`
  parsed `--name-status` (no `-z`) by whitespace-splitting, truncating paths with spaces and
  mis-handling rename records. Rewritten to `-z` NUL parsing via the testable
  `ParseGitBranchDiffNameStatusZ`. `src/project/GitCompareService.cpp`; test
  `Git/BranchDiffNameStatusZParser`.
- **SIGPIPE crash**: no handler existed, so a `write()` to a subprocess/PTY/LSP pipe whose
  reader died could terminate the editor. `platform::IgnoreBrokenPipeSignal()` is now installed
  at startup (`main.cpp`). `src/platform/HostPlatform.cpp`; test
  `Subprocess/IgnoreBrokenPipeSignalPreventsCrash`.
- **Compare `ignore_whitespace` showed wrong right-column text**: the all-equal fast path in
  `BuildCompareModel` copied the left line into `right_text`, so whitespace-only differences
  rendered the left file's text on the right. `src/compare/CompareModel.cpp`; test
  `Compare/IgnoreWhitespacePreservesRightText`.
- **Plugin Lua stack leak (hot paths)**: `QueryCompletions` / `QueryCodeActions` left the pushed
  function + args on the Lua stack when `find_plugin_by_state` returned null (PCall skipped),
  accumulating across keystrokes toward stack overflow. They now `lua_settop` back to the captured
  base on that branch. `src/plugin/PluginProviderQueryInterop.cpp`.

### Deferred — verified, still open (next pass)

- **Plugin Lua stack leak (cold paths)**: ~~the same null-plugin early-return leak exists in the
  remaining provider/hover/scm/auth/test-discovery functions.~~ **Closed on 2026-06-11 — see §23.**
- **Snippet linked-placeholder column desync**: ~~editing one of several same-line linked
  placeholder ranges does not shift the others.~~ **Closed on 2026-06-11 — see §23.**
- **Blame cache can become eligible-but-empty after self-eviction**: ~~`GitBlameService.cpp` uses
  `file_caches[key]` after `EnforceCacheBudgets()` may have evicted `key`.~~ **Closed on
  2026-06-11 — see §23.**
- **Duplicated hit-test geometry across cursor / click / motion / render TUs**: bottom-panel
  line-at-point, compare collapsed-context row + action buttons, per-mode sidebar header buttons,
  empty-tab-strip placeholder rect, and the tab-strip overflow/tab/close walk are each re-implemented
  in 2–3 TUs (already diverging on truncate-vs-floor). Centralize into shared helpers
  (`WorkspaceLayout.h` / `CompareMergeRender.h`). See the workspace audit for exact file:line sets.
- **Settings/Help overlay does not trap focus**: ~~`SettingsOverlayService` only consumes Escape;
  other keys edit the surface underneath. Fold it into the shared overlay/focus ownership.~~
  **Closed on 2026-06-11 — see §22.**
- **Divergent git status-priority tables**: ~~`GitStatusService::BuildGitStatusMap` and
  `GitPorcelainParser::GitStatusPriority` rank `Added`/`Untracked` differently.~~ **Closed on
  2026-06-11 — see §23.**

## 19. 2026-06-11 Plugin Lua-error longjmp safety pass

Closed the deferred "`luaL_error` longjmp over C++ destructors" item from §18, and the fix turned
out to be broader than first scoped. Raising a Lua error is a C `longjmp` (the project links the C
build of Lua, `liblua5.4`), so it unwinds the entire native stack back to the enclosing protected
call **without running any C++ destructor in between** — undefined behaviour and a leak whenever a
`std::string` / `std::vector` / `std::filesystem::path` is alive on any intervening frame.

ASAN proved the leak was not only in the interop functions' own locals but also in the thin
`PluginHostLuaApi.inc` lua_CFunction **wrappers**: their `host ? host->member : T{}` null-host
fallbacks materialized a `std::filesystem::path` / `PluginHost::Callbacks` temporary that the inner
`longjmp` skipped.

Fixed comprehensively:

- New `src/plugin/LuaError.h`: `lua_error_util::PushMessage` (copies the message into Lua memory so
  the source `std::string` may destruct first) and the `kPendingError` sentinel.
- Delegating TU functions (`process_interop::LuaProcessRun`/`RunAsync`,
  `runtime_api_interop::LuaDiagnosticsPublish`/`Clear`) are now **longjmp-free**: argument validation
  uses `lua_type`/`lua_isstring` (not `luaL_*`), and on error they `PushMessage` + return
  `kPendingError`. `LuaProcessRun` parses into a scoped `ProcessRunArgs` struct so every heap local
  destructs before any raise.
- The `.inc` wrappers raise (`lua_error`) only after their own locals destruct, and bind the null-host
  fallbacks by reference via `EmptyProjectRoot()` / `EmptyCallbacks()` so **no wrapper-frame
  temporary** exists to leak.
- `workspace_interop::LuaWorkspaceOpenFile` reordered so its `luaL_optinteger` calls precede the
  `std::filesystem::path` local.
- The `.inc` registration helpers (`Register*Contribution`, `LuaCommandsAdd`/`SidebarAdd`/`HoverAdd`)
  scope their `error_message` and raise after it destructs.

Validated under the ASAN preset (`PluginHost/ProcessRunReportsArgumentErrorsWithoutCorruptingState`
exercises process/diagnostics/files/workspace error paths and a follow-up success to prove the Lua
stack stays intact). New hard invariant `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` bans
`luaL_error` in `src/plugin` so the unsafe shape cannot return; entry-only `luaL_check*` stays
allowed. Note: this pass required installing `liblua5.4-dev` locally so `MICROIDE_HAS_LUA_PLUGINS=1`
— the plugin code (and the §18 `PluginProviderQueryInterop` hot-path fix) is compiled out when Lua
dev headers are absent, so always validate plugin work with Lua enabled.

## 20. 2026-06-11 Recursive-scanner symlink-loop guard

Closed the deferred "recursive scanners have no symlink-loop guard" item from §18. Both
`ProjectFileScanner::CollectFiles` and `DirectoryTree::AppendDirectory` recurse into child
directories, and a directory symlink whose real target is an ancestor (`loop -> .`, or a mutual
`a/p -> b`, `b/q -> a` pair) turned the directory tree into a cycle. The file scanner recurses
unconditionally, so an ancestor-referential symlink anywhere under the project root drove unbounded
recursion — a stack-overflow crash or hang on simply opening such a project.

A real directory can never be its own ancestor (POSIX forbids hard-linked directories), so every
cycle must cross at least one directory *symlink* and will repeat that symlink's real target
infinitely. The fix exploits that: new header-only `src/project/SymlinkLoopGuard.h` records the
canonical targets of the symlinks followed on the current descent branch and refuses to descend when
a symlink resolves to a target already on the branch (or is broken/inaccessible). Non-symlink
directories enter unconditionally and record nothing, so the common case pays no `canonical()` cost;
the guard's `Scope` is RAII so sibling branches may still legitimately follow a symlink to the same
real directory once each. Both scanners now thread a `SymlinkLoopGuard&` through their recursion and
gate each directory descent on `loop_guard.TryEnter(path, is_symlink).entered()`.

Regression coverage: `ProjectFileScanner/TerminatesOnSymlinkLoop` (a `sub/loop -> root` cycle: the
scan terminates and indexes the real file exactly once) and `DirectoryTree/StopsExpandingSymlinkCycle`
(a `loop -> root` symlink: following it once materializes its children, but expanding `loop/loop`
does not re-enter the cycle). Validated under the regular and ASAN presets.

## 21. 2026-06-11 Subprocess deadlock + terminal fd-lifecycle hardening

Closed the deferred "`RunSubprocessWithBackend` large-stdin deadlock" and
"`PosixAsyncProcessBackend` / `PosixTerminalBackend` fd lifecycle races" items from §18.

**Dead code removed.** `src/platform/ProcessBackend.cpp` / `.h` (`RunSubprocessWithBackend`,
`AsyncProcessBackend`, `PosixAsyncProcessBackend`, `CreateAsyncProcessBackend`) had **zero
consumers** anywhere in `src/`, `tests/`, or `tools/`, and duplicated ~190 lines of pipe/fd helpers
verbatim from the live `src/platform/Subprocess.cpp`. Deleting both files (and the CMake entry)
resolves the `PosixAsyncProcessBackend` fd-race outright and removes the dead twin of the stdin
deadlock. The live subprocess path is `Subprocess.cpp::RunSubprocess`.

**Live stdin deadlock fixed.** `RunSubprocess` wrote *all* of stdin synchronously
(`WriteAllToPipe`) before draining captured stdout/stderr. A child that filled its stdout pipe
(~64 KiB) before consuming stdin would block on `write(stdout)` while the parent blocked on
`write(stdin)` — a classic pipe deadlock. Replaced the sequential write-then-drain with a single
non-blocking `poll()` pump (`PumpChildIo`) that feeds stdin (`POLLOUT`) while concurrently draining
stdout/stderr (`POLLIN`); all three pipe ends are set `O_NONBLOCK` and `DrainReadyPipe` now treats
`EAGAIN` as "buffer drained" rather than blocking. Interleaving the two directions cannot deadlock.
Regression: `Subprocess/LargeStdinDoesNotDeadlock` echoes a 4 MiB payload through `cat` (a hang,
caught by the ctest timeout, is the pre-fix failure mode).

**Terminal fd race fixed.** `PosixTerminalBackend::Stop()` (`src/platform/TerminalBackend.cpp`)
`close()`d `master_fd` to interrupt the reader thread's `poll()`/`read()` — closing an fd another
thread is actively polling is a data race and risks fd-number reuse. Added a self-pipe: the reader
now polls `master_fd` plus a wake fd, `Stop()` wakes it by writing one byte, reaps the child, joins
the reader, and only *then* closes `master_fd` (when the reader has provably stopped touching it).
`master_fd_` became `std::atomic<int>` so owner-thread `Write`/`Resize`/`Start`/`Stop` accesses are
race-free. The reader's `poll()` lost its 100 ms timeout (the wake fd interrupts it directly), so an
idle terminal no longer wakes ten times a second — a small low-CPU win. Existing terminal tests
(81) plus the subprocess suite pass clean under the regular, ASAN, and **TSAN** presets.

## 22. 2026-06-11 Overlay focus & dismissal correctness (round 2)

A follow-up UI/UX correctness pass after the 2026-06-11 focus-stranding work (§18 "Keyboard-focus
stranding"), surfaced by a fresh fan-out audit across editor/platform/project/plugin/workspace. Two
overlay focus/dismissal defects landed; the fan-out also produced one finding that was **disproved**
on inspection (recorded below so it is not re-investigated).

### Fixed in this pass (with regression coverage)

- **Commit picker stayed painted over the comparison it opened** *(fresh — not previously in §18)*.
  In `WorkspaceShell::ActivateOverlaySelection` (`src/workspace/WorkspaceShellOverlay.cpp`), the
  `CommitPicker` case called `OpenSelectedCompareCommit()` then `return true` with **no
  `DismissOverlay`** — every sibling activation case (`FileFinder`, `BufferSearch`) dismisses. Traced
  through `OpenSelectedCommit → OpenComparison → DiffTabCoordinator::OpenComparison`: nothing
  dismissed the picker, so after pressing Enter the picker overlay remained on top of the freshly
  opened diff and had to be cleared manually with Escape. Fix: `DismissOverlay(true)` after the open
  (focuses the new compare tab, matching FileFinder/BufferSearch). Test
  `WorkspaceShell/CommitPickerDismissesAfterOpeningCompare` (real git repo with file history: open
  picker → Enter → assert overlay hidden **and** the active tab is the comparison).
- **Settings/Help overlay did not trap keyboard focus** *(§18)*. The overlay (a `SettingsOverlayService`,
  *not* part of `current_project_state.overlay`) set no `surface.focus`, and the key path consumed
  only Escape — every other key fell through to `HandleDefaultEditorKeyDown` and **edited the buffer
  underneath** (Enter inserted a newline, Backspace deleted, arrows moved the caret). Worse, because
  the service isn't `state_.overlay`, `editor_chord_allowed` (`!state_.overlay.visible`) stayed true,
  so editor keybindings/chords could fire too. Fix: a dedicated modal trap at the top of
  `KeyInputCoordinator::HandleKeyDown` (before `HandleGlobalKeyDown`, so global shortcuts/chords are
  swallowed as well) — while `settings_overlay_visible()` is true, Escape closes the overlay and
  every other key returns handled. The now-redundant Escape branch in `HandleSurfaceNavigationKeyDown`
  was removed. Mouse interaction is unchanged. Test
  `WorkspaceShell/SettingsOverlayTrapsKeyboardInput` (open settings over a file → Enter/Backspace/Down
  are all consumed and the editor buffer + caret are unchanged → Escape closes).

Both changes are pure UI control-flow (no new allocations, ownership, or threading). Validated under
the regular preset: the two new tests plus the surrounding `WorkspaceShell` / `Compare` / `KeyInput`
suites (348 focused, full `microide_tests` green) and the `ArchitectureInvariants` lint all pass. A
new test-access helper `OpenComparePickerForPath` and `ActiveTabIsCompare` back the commit-picker
test. (One unrelated intermittent failure, `WorkspaceLspClient/DidOpenQueuedBeforeInitializeStillDeliversFullText`,
was observed once in a full serial run and passed on isolated and repeat full runs — LSP-queue
timing, not from this pass; consistent with the §12.2-style watchlist.)

### Disproved by inspection (do not re-investigate)

- **"Compare/Merge Escape closes the tab before dismissing an open overlay."** Not a bug. When any
  `state_.overlay` overlay is visible the focus-stranding fix keeps `surface.focus == Overlay`, so
  `KeyInputCoordinator::HandleKeyDown` dispatches to `HandleOverlayKeyDown` at the `focus == Overlay`
  branch and returns **before** the compare/merge editor handlers (which carry the
  `Escape → request_close_active_tab` binding) are ever reached. The tab-close Escape only runs when
  no overlay is open, which is correct.

### Still open after this pass (verified, ranked)

Items 1–4 below were **closed on 2026-06-11 in the §23 correctness batch**. The remaining open item:

1. **Duplicated hit-test geometry across cursor/click/motion/render TUs** (dedup; medium). Bottom-panel
   line-at-point, compare collapsed-context row + action buttons, per-mode sidebar header buttons,
   empty-tab-strip placeholder rect, and the tab-strip overflow/tab/close walk are each re-implemented
   in 2–3 TUs (already diverging on truncate-vs-floor). Centralize into shared helpers
   (`WorkspaceLayout.h` / `CompareMergeRender.h`). The only remaining §18-derived correctness/dedup
   item; the largest of the batch, deferred as its own pass.

### Unmeasured perf/dedup candidates (do NOT touch without a perf fixture first)

A perf-focused fan-out flagged the following, but they are **unmeasured** and this codebase has a
documented history of "obvious" optimizations regressing the gate (§13 glyph atlas, §15 rejected
`TextDocumentModel`). Treat as hypotheses; gate any work on a `tools/perf-compare.py` fixture that
actually surfaces the cost before committing effort. Most of this class historically sits in the
noise band.

- Terminal render re-checks selection membership per cell per frame in
  `WorkspaceShellRenderBottomPanel.cpp` (background + foreground passes) when a selection is active.
- `ComputeVisibleTabs` / `ComputeVisibleProjectTabs` recomputed several times per frame across the
  chrome render + tooltip paths (`WorkspaceShellRenderChrome.cpp`) — identical results within a frame.
- Per-visible-row `TruncateLabel` string allocations in compare summary rendering
  (`WorkspaceShellCompareRender.cpp`); candidate for a reused scratch buffer / `string_view` slicing.
- Sidebar header button rects recomputed twice per mouse-motion event in
  `WorkspaceShellMouseMotion.cpp` (previous + current hover) without a layout-revision cache.

## 23. 2026-06-11 Correctness batch: snippet mirror, git status priority, blame cache, Lua cold paths

Closed four of the five remaining §18/§22 correctness items in one pass. Each fix carries regression
coverage except where noted; the regular suite plus the ASAN preset (the §19-mandated validation for
plugin Lua work, with `MICROIDE_HAS_LUA_PLUGINS=1`) are green.

- **Snippet linked-placeholder column desync** (`src/editor/SnippetEngine.cpp`). `SnippetTryInsertText`
  bumped only the edited range's `end.column`; same-line sibling occurrences to the right of the
  insertion kept stale columns, so the *second* mirrored keystroke into a multi-occurrence snippet
  landed at the wrong column. The insertion shifts every column at or after the insertion point, so
  the fix now advances **both** `start.column` and `end.column` of every sibling range on the line
  whose start is at/after the insertion. Test
  `EditorSnippet/MultiOccurrenceLinkedTabMultiKeystroke` types three successive characters into a
  two-occurrence tab stop and asserts `x!?. y!?.`.

- **Divergent git status-priority tables** (`src/project/GitStatusService.cpp`).
  `BuildGitStatusMap` carried its own inline priority table ranking `Added == Untracked == 2`, while
  the canonical `GitPorcelainParser::GitStatusPriority` ranks `Added = 2 > Untracked = 1`; folder-badge
  aggregation therefore depended on which producer ran. `BuildGitStatusMap` now delegates the entire
  file + parent-folder aggregation to `GitPorcelainParser::RecordGitStatus` (which the function had
  been duplicating verbatim), so the ranking is single-sourced and the duplication is gone. Test
  `Git/BuildStatusMapFolderPriorityIsSingleSourced` proves a folder holding both an Added and an
  Untracked file aggregates to `Added` regardless of entry order.

- **Blame cache eligible-but-empty after self-eviction** (`src/project/GitBlameService.cpp`). The
  post-span-loop re-validation block used `file_caches[request.file_key]` (operator[]). If the loop's
  `EnforceCacheBudgets()` had just evicted that key (a single file whose blame exceeds
  `kMaxCachedLines`, or eviction mid-loop), operator[] resurrected it as a default cache and then set
  `eligible = true` — an empty-but-eligible entry that wastes the reclaimed slot and reports
  ready-with-no-data to readers. The block now `find()`s the entry and re-validates only if it
  survived. No deterministic regression test was added: triggering eviction of the *current* request
  needs either a >16 000-line blame fixture or a test-only cache-budget setter (the current file is
  always most-recently-used, so normal LRU never evicts it); the existing blame suite covers the
  apply path for no-regression. A budget-hook-driven test is a small deferred follow-up.

- **Plugin Lua stack leak (cold paths)** (`src/plugin/PluginProviderQueryInterop.cpp`,
  `src/plugin/PluginSidebarHoverInterop.cpp`). The remaining provider/test/scm/auth/mcp/command/
  save-participant/sidebar/hover functions shared the §18 hot-path bug: when `find_plugin_by_state`
  returns null the protected call is skipped, leaving the pushed function + arguments on the Lua
  stack to accumulate across queries toward overflow. Rather than scatter `lua_gettop`/`lua_settop`
  pairs (which pushed `PluginProviderQueryInterop.cpp` over the hard 800-line plugin-TU cap), this
  introduces a header-only RAII `lua_interop::StackResetGuard` (`PluginLuaInterop.h`) that restores
  the stack height on *every* scope exit — success or failure. All push-then-call functions in both
  interop TUs (including the two previously hot-patched ones, now unified) declare one right after
  acquiring the provider state; the explicit failure-branch `lua_settop` calls were removed. The file
  dropped to 793 lines, back under the cap. Validated under ASAN via
  `PluginHost/ProcessRunReportsArgumentErrorsWithoutCorruptingState` and the plugin suite; the
  `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` invariant still passes (the guard uses
  `lua_settop`, never `luaL_error`).

Remaining from the §18 audit: only the **duplicated hit-test geometry** dedup (medium; its own pass).
