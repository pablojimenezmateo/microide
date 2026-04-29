# MicroIDE Known Tech Debt

Reviewed on 2026-04-23. Updated 2026-04-29 after comprehensive tech-debt cleanup slices.

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
- merge result text always serializing with `\n`
- project search rescanning disk on every run instead of consuming an indexed snapshot
- project search allocating a lowercase copy of every candidate line in case-insensitive mode
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

## 5. Search and Index Integration Is Better, but Still Snapshot-Based Rather Than Event-Driven

Impact:
- Medium
- Lower urgency than the pre-`0aa44cb` search issues, but still worth tracking

Current state:
- Project search can now consume an indexed file snapshot instead of rescanning the tree itself.
- `FileIndex` now caches hidden and non-hidden scans separately.

What is still debt:
- Search still consumes a point-in-time file list and then opens files directly from disk.
- There is no deeper shared project-content service for search, file finder, diagnostics-driven
  refresh, and other read-heavy workflows.
- The index refresh policy is still explicit and host-driven rather than fully unified with all
  project mutations and file-watch events.

Evidence:
- `src/project/FileIndex.h`
- `src/project/FileIndex.cpp`
- `src/project/ProjectSearchService.cpp`
- `src/workspace/WorkspaceShellProjectSearch.cpp`

Why this is still debt:
- The biggest search inefficiencies were fixed, but project read paths are still not unified under
  one host-owned content model.
- As plugin and indexing workloads grow, the project layer may want a narrower “project snapshot”
  or content service instead of multiple consumers reading the filesystem independently.

Recommended follow-up:
- Only pursue this if profiling shows file-discovery or file-open churn is still material after the
  recent fixes.

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

References:
- `docs/startup-tracing.md`
- `docs/runtime-profiling.md`
- `docs/performance-findings.md`

Recommended follow-up:
- Add or extend focused benchmarks where repeated regressions are likely:
  - large-file cursor jump and initial paint
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
- Chat composer remains multiline by design and is tracked separately as feature work, not debt.

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

## Suggested Order For Later Work

1. Keep shrinking `WorkspaceShell` and reduce friend-based coordinator access.
2. Finish the active-viewport migration so stale floating-editor paths are harder to reintroduce.
3. Build one shared selection-aware editor model for single-line shell inputs.
4. Revisit project-content and indexing architecture only if profiling still shows meaningful
   search or refresh cost after the current fixes.
