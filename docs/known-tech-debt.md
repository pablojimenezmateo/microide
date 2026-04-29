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

## 1. `WorkspaceShell` Is Smaller Than Before but Still the Main Ownership Bottleneck

Impact:
- High
- This remains the main architectural debt in the workspace layer

Current state:
- The shell is much better factored than earlier revisions.
- Input, rendering, persistence, hover, compare, merge, sidebar, and project search all have
  dedicated translation units.
- The active editor-state bug was reduced by moving action and render paths toward the
  tab-owned active viewport.

What is still wrong:
- `WorkspaceShell.h` is still 1514 lines.
- `WorkspaceActionContext.cpp` is still 694 lines and remains a broad façade over shell internals.
- The shell still exposes a large private surface to many coordinators through friendship.

Evidence:
- File size:
  - `src/workspace/WorkspaceShell.h` is currently 1514 lines
  - `src/workspace/WorkspaceActionContext.cpp` is currently 694 lines
- Friend-based access remains broad:
  - `src/workspace/WorkspaceShell.h:740-760`
  - `src/workspace/WorkspaceShell.h:1510`

Why this is still debt:
- Many subsystems are now physically split into separate `.cpp` files, but the shell is still the
  object through which most ownership and mutation flow.
- The result is better organization without equally strong API boundaries.
- New work still tends to require one more helper or one more friend path instead of a narrow
  subsystem contract.

What a good fix likely looks like:
- Continue moving state and mutation behind subsystem-owned APIs, not just separate translation
  units.
- In particular, keep shrinking direct shell access in:
  - action execution
  - editor or split manipulation
  - sidebar open-or-select flows
  - prompt and path-mutation flows
  - compare or merge navigation commands
- Treat `WorkspaceActionContext` as a transitional façade, not a final architecture.

Likely starting files:
- `src/workspace/WorkspaceShell.h`
- `src/workspace/WorkspaceActionContext.cpp`
- `src/workspace/WorkspaceTabCoordinator.cpp`
- `src/workspace/WorkspaceSidebarCoordinatorActions.cpp`
- `src/workspace/WorkspacePathMutationCoordinator*.cpp`

Recommended follow-up:
- Reduce the number of `friend class` entries over time instead of adding new ones.
- Prefer shell-owned narrow services and registries over more helper methods on the shell itself.

## 2. Coordinator Separation Is Still Partly Superficial

Impact:
- Medium
- This is closely related to shell size, but worth tracking separately because it affects review
  quality and extension safety

What still happens:
- Many coordinators are now top-level types, but they still operate by reaching directly into
  shell internals rather than consuming stable subsystem APIs.
- The refactor improved file ownership, but some behavior still depends on privileged write access
  into the shell’s private state.

Evidence:
- Coordinators with direct friend access:
  - `WorkspaceActionContext`
  - `ProjectCatalogCoordinator`
  - `PersistenceCoordinator`
  - `CommandPromptCoordinator`
  - `MenuCoordinator`
  - `KeyInputCoordinator`
  - `TextInputCoordinator`
  - `TabCoordinator`
  - `PathMutationCoordinator`
  - `LifecycleCoordinator`
  - `DirtyPromptCoordinator`
  - `CompareInteractionCoordinator`
  - `DiffTabCoordinator`
  - `SidebarCoordinator`
  - `ChromeMouseCoordinator`
  - `EditorMouseCoordinator`
  - `CompareMouseCoordinator`
  - `MergeMouseCoordinator`
  - `TabMouseCoordinator`
  - `SidebarMouseCoordinator`
  - `PanelMouseCoordinator`
- See `src/workspace/WorkspaceShell.h:740-760`

Why this is still debt:
- It is hard to tell which invariants are actually protected by APIs and which ones are just
  social conventions.
- Plugin-phase work benefits from explicit host boundaries, and this pattern keeps those
  boundaries looser than they should be.

What a good fix likely looks like:
- Replace direct friend access with narrower shell-owned service objects or focused mutation APIs.
- Make coordinators ask for operations such as “activate split”, “open diagnostic result”, or
  “persist current project state” instead of editing unrelated shell fields directly.
- Keep rendering host-owned, but keep command/navigation requests separate from raw shell state.

Recommended follow-up:
- When touching a coordinator for another reason, check whether one or two direct field accesses
  can be replaced with a dedicated API instead of preserving the friend pattern by default.

## 3. Active Editor Viewport Ownership Migration

Status:
- Addressed on 2026-04-29 by removing `text_viewport_` alias paths and routing editor actions through tab-owned active viewport APIs

Impact:
- Medium
- The high-risk stale-state bug class was reduced, but this is still an area to watch

Current state:
- The major user-facing paths now use `ActiveEditorViewport()` or `ActiveNavigableViewport()`.
- Full tests passed after fixing several remaining stale-path callers.

What remains:
- Keep validating active-viewport ownership through regression tests when adding editor or split behavior.

Evidence:
- Active viewport helpers:
  - `src/workspace/WorkspaceShell.h`
  - `src/workspace/WorkspaceShellEditor.cpp`
- The legacy alias has been removed; use service-owned active viewport accessors only.

Why this is still debt:
- The architecture is safer than before, but not yet simplified enough that the wrong path is
  impossible.
- This remains a subtle regression surface for splits, persistence, blame overlays, and
  open-or-navigate commands.

What a good fix likely looks like:
- Decide whether the project-level `text_viewport_` should remain only as a welcome or placeholder
  buffer, or whether it should be removed entirely from normal editor-tab ownership.
- If it stays, document exactly when it is authoritative and when it is not.
- If it goes, move any remaining placeholder or startup responsibilities onto a dedicated model.

Recommended follow-up:
- Prefer `ActiveEditorViewport()` and `ActiveNavigableViewport()` in all new code.
- Treat new direct `text_viewport_` mutations in editor workflows as a code-review smell.

## 4. Render and Hover Paths Still Reach Widely Through the Shell

Impact:
- Medium
- This is now more of an architectural cleanliness problem than an urgent bug

Current state:
- Top-level render work is split across dedicated files.
- The recent viewport-ownership patch corrected several active-pane lookups in render and hover
  paths.

What is still debt:
- Render and hover code still depends on broad shell helper reach rather than narrow renderer
  inputs.
- The shell still orchestrates nearly every surface directly, which keeps UI behavior tightly
  coupled to shell state layout.

Evidence:
- Render files:
  - `src/workspace/WorkspaceShellRenderFrame.cpp`
  - `src/workspace/WorkspaceShellRenderOverlay.cpp`
  - `src/workspace/WorkspaceShellRenderTextInput.cpp`
- Hover files:
  - `src/workspace/WorkspaceShellHoverTargets.cpp`
  - `src/workspace/WorkspaceShellHoverPopup.cpp`

Why this is still debt:
- The current split improves readability, but UI bugs can still require multi-surface shell edits.
- This is especially relevant if plugin-provided sidebars, diagnostics, or hover providers grow in
  scope.

What a good fix likely looks like:
- Continue moving per-surface rendering toward narrower data inputs.
- Avoid adding new render-time shell queries when a surface-specific view model would do.
- Keep all actual drawing host-owned, but make the dependency graph smaller.

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
- Medium
- Text insertion, caret painting, IME composition, and paste routing are standardized, but basic
  editing behavior still depends too much on per-surface handlers

What remains:
- Chat composer still uses the multiline viewport model by design and should only migrate shared behavior where semantics match.

Why this is still debt:
- New single-line surfaces can still reintroduce behavior drift even though rendering is now
  standardized
- Shortcut parity and menu parity remain easier to regress than they should be
- Plugin growth will increase the number of prompt-like surfaces over time, which raises the cost
  of duplicated line-edit logic

What a good fix likely looks like:
- Introduce one host-owned single-line editor state with:
  - buffer text
  - caret position
  - optional selection range
  - shared helpers for backspace, delete-forward, left or right movement, home or end, and
    select-all
- Keep submit, history, and other surface-specific actions outside that shared editor model
- Extend tests so keyboard shortcuts and menu actions exercise the same single-line editing path

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
