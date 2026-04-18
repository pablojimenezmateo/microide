# Production Tech Debt Review

Reviewed on 2026-04-14.

Scope:
- `src/app/*`
- `src/project/*`
- `src/workspace/*`
- non-test production code only

This document records the next meaningful production-code debt after the recent compare, merge,
window-presentation, layout-constant, and git argv-runner cleanup passes.

## Highest-Impact Findings

### 1. `WorkspaceShell` is still the main blast-radius object

Impact:
- High
- Small features and bug fixes still require touching one class that owns too many unrelated
  concerns.

Evidence:
- `WorkspaceShell` still owns window presentation, project catalog state, tree/index/finder,
  editor tabs, terminal tabs, overlays, git sidebar, prompts, clipboard hooks, cursor state, and
  dialog plumbing.
- Shell redraw invalidation, caret timing, clipboard or text-input routing, cursor hit-testing,
  and breadcrumb or project-tab presentation now live in dedicated
  `WorkspaceShellRedraw.cpp`, `WorkspaceShellInteraction.cpp`, `WorkspaceShellCursor.cpp`, and
  `WorkspaceShellPresentation.cpp` units instead of staying mixed into `WorkspaceShell.cpp`.
- Key-input dispatch still targets the shell, but modal or menu, surface, and editor-domain
  handling now live in dedicated `WorkspaceKeyInputCoordinator*` translation units instead of one
  monolithic coordinator file.
- Sidebar mode switching still targets the shell, but refresh and entry-action handling now live
  in dedicated `WorkspaceSidebarCoordinator.cpp`, `WorkspaceSidebarCoordinatorRefresh.cpp`, and
  `WorkspaceSidebarCoordinatorActions.cpp` units instead of one monolithic coordinator file.
- Sidebar mouse dispatch still targets the shell, but per-surface button handling and scroll
  routing now live in dedicated `WorkspaceSidebarMouseCoordinator.cpp` and
  `WorkspaceSidebarMouseCoordinatorScroll.cpp` units instead of one catch-all mouse coordinator
  file.
- Top-level mouse routing still targets the shell, but button lifecycle handling now lives apart
  from motion and wheel flow in dedicated `WorkspaceShellMouse.cpp` and
  `WorkspaceShellMouseMotion.cpp` units instead of one catch-all shell mouse file.
- Persistence still targets the shell, but config/theme state, project-session state, and
  workspace-session state now live in dedicated `WorkspacePersistenceCoordinator.cpp`,
  `WorkspacePersistenceCoordinatorConfig.cpp`,
  `WorkspacePersistenceCoordinatorSession.cpp`, and
  `WorkspacePersistenceCoordinatorWorkspaceSession.cpp` units instead of one large coordinator
  file.
- The shell private surface still spans action dispatch, input handling, render helpers, compare,
  merge, persistence, and terminal behavior.

References:
- `src/workspace/WorkspaceShell.h:35`
- `src/workspace/WorkspaceShell.h:636`
- `src/workspace/WorkspaceShell.h:1446`
- `src/workspace/WorkspaceShellRedraw.cpp`
- `src/workspace/WorkspaceShellInteraction.cpp`
- `src/workspace/WorkspaceShellCursor.cpp`
- `src/workspace/WorkspaceShellPresentation.cpp`

Why this matters:
- Ownership is clearer than before, but the shell remains the place where multiple subsystems meet
  without a narrow boundary.
- That keeps the regression surface high and makes seemingly local changes expensive to review.

Recommended next step:
- Continue splitting shell-owned state and APIs by subsystem, starting with project/editor/panel/
  sidebar state objects or a narrower shell-core facade.

### 2. The helper split is no longer the main blocker

Impact:
- Medium
- `WorkspaceShellShared.*` is gone, so the next architectural bottleneck is the still-broad
  shell-owned state and coordinator surface rather than helper-file coupling.

Evidence:
- `WorkspaceLayout*` and `WorkspaceTerminalSelection*` now own layout, scroll, compare-or-merge
  marker, and terminal-selection responsibilities.
- `WorkspaceTextSearch*` now owns UTF-8 helpers, line serialization, whitespace normalization,
  and literal-search helpers.
- `WorkspaceCommandParsing*` now owns command-line parsing, completion, and ui-scale text
  parsing.
- `WorkspacePathUtils*` now owns relative-path and path-prefix helpers.
- `WorkspaceProjectPresentation*` now owns project-state naming, tab-or-breadcrumb presentation,
  and project color or accent helpers.
- `WorkspaceGitSidebarPresentation*` now owns git-sidebar line and entry presentation helpers.
- `WorkspaceProjectSearchPresentation*` now owns project-search result line-map helpers.
- `WorkspaceShellShared.*` has been deleted instead of being kept as a shrinking compatibility
  bucket.

References:
- `src/workspace/WorkspaceLayout.h`
- `src/workspace/WorkspaceLayout.cpp`
- `src/workspace/WorkspaceTerminalSelection.h`
- `src/workspace/WorkspaceTerminalSelection.cpp`
- `src/workspace/WorkspaceTextSearch.h`
- `src/workspace/WorkspaceTextSearch.cpp`
- `src/workspace/WorkspaceCommandParsing.h`
- `src/workspace/WorkspaceCommandParsing.cpp`
- `src/workspace/WorkspacePathUtils.h`
- `src/workspace/WorkspacePathUtils.cpp`
- `src/workspace/WorkspaceProjectPresentation.h`
- `src/workspace/WorkspaceProjectPresentation.cpp`
- `src/workspace/WorkspaceGitSidebarPresentation.h`
- `src/workspace/WorkspaceGitSidebarPresentation.cpp`
- `src/workspace/WorkspaceProjectSearchPresentation.h`
- `src/workspace/WorkspaceProjectSearchPresentation.cpp`

Why this matters:
- It removes a source of unrelated coupling and clarifies the next real debt target.
- It also means future refactors should focus on shell ownership, registries, and service seams
  instead of reviving shared helper files.

Recommended next step:
- Keep the new helper seams cohesive and shift the next structural work toward narrower shell-owned
  sidebar, surface, and coordinator state.

### 3. Action handling is still too centralized in the shell

Impact:
- Medium
- Action ownership is clearer than before, but execution still depends on a shell-owned
  coordinator with broad access to unrelated shell internals.

Evidence:
- Built-in action ids and metadata now live in `WorkspaceActionTypes.*` and
  `WorkspaceCommandRegistry.*` instead of `WorkspaceShell`.
- Action request parsing now lives in `WorkspaceActionRequests.*`.
- Dispatch is now split by project, sidebar, search, tab, edit, and global domains across
  dedicated executor translation units, but those executors are still methods on the
  shell-owned `WorkspaceShell::ActionCoordinator`.

References:
- `src/workspace/WorkspaceActionTypes.h`
- `src/workspace/WorkspaceCommandRegistry.cpp`
- `src/workspace/WorkspaceActionRequests.cpp`
- `src/workspace/WorkspaceActionCoordinator.cpp`
- `src/workspace/WorkspaceProjectActionExecutor.cpp`
- `src/workspace/WorkspaceSidebarActionExecutor.cpp`
- `src/workspace/WorkspaceSearchActionExecutor.cpp`
- `src/workspace/WorkspaceTabActionExecutor.cpp`
- `src/workspace/WorkspaceEditActionExecutor.cpp`
- `src/workspace/WorkspaceGlobalActionExecutor.cpp`

Why this matters:
- Registration, parsing, and file ownership are better than before, but action behavior still
  scales with shell complexity because execution keeps broad write access to shell state.
- That limits how much plugins or future built-ins can reuse narrower subsystem contracts.

Recommended next step:
- Continue moving project, sidebar, search, tab, edit, and global behavior behind narrower
  subsystem-owned services or facades, and keep shrinking the shell’s remaining state exposure
  rather than rebuilding a new core catch-all around the fresh helper seams.

### 4. The main render path is still too wide

Impact:
- Medium
- Render ownership is clearer than before, but the shell still owns broad sidebar, overlay,
  bottom-panel, menu, and prompt surface state through one state-heavy shell model.

Evidence:
- Top-level render orchestration is now split across `WorkspaceShellRender.cpp`,
  `WorkspaceShellRenderFrame.cpp`, `WorkspaceShellRenderChrome.cpp`,
  `WorkspaceShellRenderSidebar.cpp`, `WorkspaceShellRenderOverlay.cpp`,
  `WorkspaceShellRenderBottomPanel.cpp`, `WorkspaceShellRenderMenus.cpp`,
  `WorkspaceShellRenderPrompts.cpp`, and `WorkspaceShellRenderTextInput.cpp`.
- Sidebar, overlay, bottom-panel, menu, and prompt drawing now live in dedicated translation
  units, but they still operate directly on shell-owned surface state and helper methods.

References:
- `src/workspace/WorkspaceShellRender.cpp`
- `src/workspace/WorkspaceShellRenderFrame.cpp`
- `src/workspace/WorkspaceShellRenderChrome.cpp`
- `src/workspace/WorkspaceShellRenderSidebar.cpp`
- `src/workspace/WorkspaceShellRenderOverlay.cpp`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp`
- `src/workspace/WorkspaceShellRenderMenus.cpp`
- `src/workspace/WorkspaceShellRenderPrompts.cpp`
- `src/workspace/WorkspaceShellRenderTextInput.cpp`

Why this matters:
- The top-level render entry point is smaller, but shell-owned render behavior still spans too
  many unrelated UI surfaces.
- UI regressions remain harder to isolate than they should be.

Recommended next step:
- Keep the new surface-specific render files narrow, then peel sidebar, overlay, bottom-panel,
  menu, and prompt state behind smaller renderer inputs or service-owned models instead of routing
  them all through `WorkspaceShell`.

### 5. The git process layer is no longer the main process-boundary debt

Impact:
- Low
- The old inline-header process concern is resolved, so future process work should focus on
  higher-level git-service behavior instead of redoing the execution seam.

Evidence:
- `GitCommandUtil.cpp` now owns git command execution and delegates to the host subprocess
  service in `platform/Subprocess.*`.

References:
- `src/project/GitCommandUtil.cpp`
- `src/platform/Subprocess.cpp`

Why this matters:
- It means the next debt here is higher-level git-service shaping, not fixing a stale low-level
  execution boundary that already moved behind compiled code.

Recommended next step:
- Keep git execution on the subprocess service and only widen or reshape it if git-specific
  workflows need better cancellation, streaming, or richer error reporting.

## Recommended Order

1. Reduce `WorkspaceShell` ownership further.
2. Decentralize action registration and dispatch.
3. Keep narrowing render and surface ownership.
4. Keep git and other external-tool integrations on narrow compiled services.
5. Keep new helper and render seams cohesive; do not reintroduce catch-all buckets.

## Non-Findings

- I did not identify a new high-priority duplicate-window-state issue; that area looks materially
  better after the recent window-presentation cleanup.
- I did not identify a new compare/merge ownership issue at the same severity as the ones already
  addressed.
