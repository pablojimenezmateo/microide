# MicroIDE Known Tech Debt

Reviewed on 2026-04-23. Updated 2026-04-29 after comprehensive tech-debt cleanup slices.
Updated 2026-05-18 with rejected refactor experiment notes.
Updated 2026-05-19 with project-search throughput and perf-compare measurement fixes.
Updated 2026-05-19 with post-`e9a4764` perf-compare null-result investigation and the
two follow-up items it surfaced (merge scrollbar marker cache, hover visual-column map).

This document records the meaningful debt that remains after commit `0aa44cb`
(`Fix shared diff/search paths and active editor state`).

Use this file for deferred work that is real, actionable, and still open.
Use `docs/active-work.md` for current priorities.
The broader architectural review (from 2026-04-20) is archived at `docs/archive/production-tech-debt-review.md`.

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
- `DirectoryTree::RefreshGitStatuses()` still runs `CollectGitStatuses` synchronously
  on the UI thread inside `WorkspaceSidebarCoordinator::ShowGit()` and on project
  set-root in `WorkspaceProjectStateCoordinator`. The 2026-05-19 attempt to migrate this
  to an async snapshot-and-apply pipeline (alongside an unconditional
  `RefreshGitSidebar()` on every project open) was reverted after the perf-compare bake:
  unconditionally posting the 4-subprocess async refresh on every project set-root
  produced ~480k extra short-lived allocations per project open with no wall-time
  benefit (see investigation note below). The async snapshot pieces (`tree_git_statuses`
  on `RefreshSnapshot`, `DirectoryTree::ApplyGitStatuses`, async-fallback in
  `RefreshGit()`) were rolled back along with the unconditional `RefreshGitSidebar`.
  Re-attempting this migration requires keeping the trigger conditional on the user
  actually wanting tree badges (e.g. tied to Git sidebar mode or an explicit user
  preference), not running it speculatively on every project open.
- Blame and log dispatch are already off the UI thread.

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
- Make `DirectoryTree::RefreshGitStatuses()` async-snapshot-and-apply, but gated on
  Git sidebar visibility (or an explicit setting) rather than running on every project
  set-root. See the investigation note above for the reverted attempt.

## 6. Large-File and Performance Validation Still Needs Measurement, Not Assumptions

Impact:
- Medium
- This is process debt with real product consequences

What is still open:
- The recent fixes remove known hotspots, but large-file behavior still needs empirical validation.
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
- The harness also still lacks a dedicated large-file open-to-first-paint gate; advisory scenario
  `large_file_open_first_paint` exists for explicit local runs only.
- Diff / merge coverage is now better on sustained interaction because the gated suite includes
  `compare_scroll_large_fixture` and `merge_scroll_large_fixture`, but it is still narrower than a
  full interaction matrix. Large-fixture hunk navigation and mixed edit/scroll traces remain open.

References:
- `docs/startup-tracing.md`
- `docs/runtime-profiling.md`
- `docs/performance-findings.md`

Recommended follow-up:
- Add or extend focused benchmarks where repeated regressions are likely:
  - large-file open to first paint / first interactive frame
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

See also `docs/performance-findings.md` — Second Performance Pass, New finding 1.

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

See also `docs/performance-findings.md` — Second Performance Pass, New finding 2.

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

See also `docs/performance-findings.md` — Second Performance Pass, New finding 3.

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

See also `docs/performance-findings.md` — Second Performance Pass, New finding 4.

## 15. `TextViewport.cpp` Ownership Concentration

Status:
- Substantially addressed on 2026-05-18 across five extractions, all keeping methods as members
  of the same `TextViewport` class (no header / API change, no friending):
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
- Result: `TextViewport.cpp` is now ~1,788 lines, down from ~3,553 — roughly a 50 % reduction
  with no public API change. The extracted `TextViewportViewState.cpp` is ~448 lines.

Remaining:
- Low to medium. The host TU no longer owns cursor + scrolling + view-state, but it still owns
  selection + single-caret edits (`Backspace`, `DeleteForward`, `InsertCharacter`, etc.),
  the history / undo machinery, invalidation policy, and the visible-line / wrapped-row layout
  caches. These are still tangled by design (undo records visual columns, selection, multi-caret,
  and view state together; the invalidation policy is the contract between every cache).

Why this item stays open at "low":
- File decomposition has done most of what it can without an ownership refactor. The next
  meaningful seams are no longer “move obvious helper clusters”; they are larger design moves
  such as extracting undo/history ownership or separating edit application from layout/cache
  invalidation. The remaining `TextViewport.cpp` is now a coherent core, not a catch-all.
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
- `EditEngine`
  - range edits
  - multi-caret edit normalization
  - auto-pair edit transforms
  - edit grouping boundaries
  - range validation
- `UndoHistory`
  - undo/redo stacks
  - grouping
  - caret/selection restore
  - revision integration
- `TextLayoutCache`
  - soft-wrap rows
  - visible-line cache
  - fold-aware mapping
  - layout invalidation

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
- Open at "low" after the 2026-05-18 audit, ratchet-only caps, and the first assist extraction.

Impact:
- Low to medium. The architectural-lint cap on `WorkspaceShell.h` (≤ 400 lines) and
  `WorkspaceShell.cpp` (≤ 600 lines) is satisfied, but behavior is still owned by the
  `WorkspaceShell` namespace through 51 `WorkspaceShell*.cpp` translation units defined against
  `WorkspaceShellMembers.inc` (~1,516 lines of inline class body). File decomposition without
  ownership decomposition keeps the shell symbol blast radius wide.

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
- Finish collapsing the remaining project/editor-tab convenience wrappers in
  `WorkspaceShellChrome.cpp` once the surrounding coordinator/test-access call sites can depend on
  the underlying services directly.
- Do not add new `WorkspaceShell*.cpp` files for new behavior — the cap now hard-fails this.

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

Source: `docs/performance-bottleneck-deep-dive-4.md` "Rejected experiment: ASCII glyph atlas".

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

Source: `docs/performance-bottleneck-deep-dive-2.md` Finding 16,
`docs/performance-bottleneck-deep-dive-3.md` partial,
`docs/performance-bottleneck-deep-dive-4.md` Finding 4 (partial).

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
  (`docs/performance-bottleneck-deep-dive-4.md`). Surface is wide — touches every
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
- Documented in `docs/runtime-profiling.md`, `guidelines/testing.md`, `AGENTS.md`, and
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
