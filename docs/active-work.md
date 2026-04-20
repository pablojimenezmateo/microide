# MicroIDE Active Work

Reviewed on 2026-04-20.

This is the single source of truth for:

- the shipped baseline that matters for ongoing work
- the current priority stack
- accepted scope cuts and deferred work

Use subsystem design docs for deep dives. Use this file to decide what is active, what is already
good enough, and what is deliberately not being built.

## Priority Order

Engineering decisions should follow this order:

1. correctness over compatibility
2. speed
3. low CPU usage
4. low memory usage
5. architectural clarity

Broad refactors are acceptable when they improve the result. Do not preserve stale boundaries,
legacy helpers, or accidental compatibility if they block correctness or performance.

## Shipped Baseline

These are implemented and should not be treated as open migration work:

- SDL3/CMake desktop shell with an event-driven render loop
- retained scene redraw path with shell-owned invalidation, coalesced dirty regions, partial-to-full promotion, and resize-safe full-redraw fallbacks for outer-layout drags
- custom menu bar, project tabs, file tabs, breadcrumbs, persistent sidebar, overlays, and docked terminal-and-command pane
- project-local workspace state plus app-level restore of open project tabs
- normal editor tabs, compare tabs, merge tabs, and nested shared-buffer splits
- editor open/save/reopen, selection, clipboard, undo/redo, line numbers, horizontal scrolling, dirty tracking, IME hooks, and project-local preferences
- editor undo and redo now store changed line ranges plus view state instead of full-buffer snapshots, and editor file open/save now reuses the shared text-file helper instead of inline stream assembly
- filesystem tree with `.gitignore` handling, git markers, refresh, and trash-backed create/rename/delete flows
- file finder overlay plus async project search with literal or regex mode, case controls, hidden-file controls, replace-in-project for literal mode, capped-result feedback, and a standalone benchmark tool
- git sidebar with compare, merge, stage, unstage, discard, outgoing-file views, bulk stage-all, and confirmed discard-all
- PTY-backed terminal tabs with scrollback, selection, copy/paste, alternate screen, title updates, OSC 52 clipboard copy, focus notifications, bracketed paste, cursor-key mode, origin mode, autowrap control, and the common ANSI scroll-region paths currently needed by real tools
- runtime syntax highlighting from the in-tree generated syntax snapshot plus plugin `syntax/*.lua` contributions loaded into the host tokenizer at startup and `plugins-reload`
- manual Lua plugin loading from user and project directories, lifecycle hooks, plugin commands, plugin sidebars, project-relative file helpers, active-buffer metadata, argv-based process helpers, repo-owned dogfood plugins, and `plugins-reload`
- plugin-published diagnostics with host-owned storage, theme-backed underline rendering, severity gutter markers, host-owned blame/diagnostic/plugin hover popups in editor surfaces, plugin hover providers, a built-in Problems sidebar, and host rename/delete cleanup for stale diagnostic paths
- targeted regression coverage across compare, merge, git services, file operations, retained redraw, workspace chrome, and plugin-adjacent registries

## Active Phases

### 1. Plugin Platform Expansion

This is the dominant current phase and will be large.

Current state:

- manual Lua plugin loading is shipped
- command and sidebar registries exist
- workspace action ids and specs plus built-in menu-bar and tree-context definitions now live in
  dedicated `WorkspaceActionTypes*` and `WorkspaceMenuRegistry*` modules instead of staying
  embedded directly in `WorkspaceShell`
- the host already exposes narrow file, workspace, process, diagnostics, and hover extension points
- host-owned app-directory, subprocess, output-channel, task-executor, and persistence-format
  services now back plugin, search, blame, and workspace state flows instead of keeping that work
  tangled in `WorkspaceShell` or `WorkspaceShellShared.*`, including argv-based plugin subprocess
  execution with cwd, stdin, and environment override support
- host-owned filesystem helpers and a host-owned tree watcher now back plugin discovery, runtime
  syntax loading, theme enumeration, and automatic plugin reload, with Linux native file-watch
  wakeups plus snapshot fallback where native coverage is not available
- plugin host lifecycle, runtime syntax reload bookkeeping, asset watching, and plugin output
  channels now run through a dedicated `WorkspacePluginRuntime*` service instead of living
  directly on `WorkspaceShell`
- workspace layout, scroll geometry, compare-or-merge marker math, and terminal-selection helpers
  now live in dedicated `WorkspaceLayout*` and `WorkspaceTerminalSelection*` modules instead of
  staying bundled into `WorkspaceShellShared.*`
- workspace UTF-8 helpers, line serialization, whitespace normalization, and literal-search
  helpers now live in dedicated `WorkspaceTextSearch*` modules instead of staying bundled into
  `WorkspaceShellShared.*`
- workspace command-line parsing, completion, ui-scale text parsing, and path-prefix helpers now
  live in dedicated `WorkspaceCommandParsing*` and `WorkspacePathUtils*` modules instead of
  staying bundled into `WorkspaceShellShared.*`
- workspace project-state naming, tab-and-breadcrumb labels, and project accent-color helpers now
  live in dedicated `WorkspaceProjectPresentation*` modules instead of staying bundled into
  `WorkspaceShellShared.*`
- workspace git-sidebar line or entry presentation and project-search result line-map helpers now
  live in dedicated `WorkspaceGitSidebarPresentation*` and
  `WorkspaceProjectSearchPresentation*` modules instead of staying bundled into
  `WorkspaceShellShared.*`
- plugin syntax contributions now load from host-owned plugin data directories and invalidate editor, compare, and merge syntax caches on reload
- workspace colorscheme, config, and session persistence now run through a dedicated persistence
  coordinator instead of keeping those flows embedded in `WorkspaceShell`
- command prompt history, completion, and command-line execution now run through a dedicated
  command-prompt coordinator instead of living directly on `WorkspaceShell`
- action dispatch for project, sidebar, search, tab, edit, and global commands now runs through a
  dedicated action coordinator instead of living in `WorkspaceShellActions.cpp`
- action request parsing now lives in `WorkspaceActionRequests*`, and the project, sidebar,
  search, tab, edit, and global action-domain implementations now live in dedicated
  `Workspace*ActionExecutor.cpp` translation units instead of one monolithic
  `WorkspaceActionCoordinator.cpp`
- the top-level action coordinator now routes project, sidebar, search, tab, edit, and global
  execution through a dedicated `WorkspaceActionContext*` facade instead of keeping action
  behavior on a nested shell-owned `WorkspaceShell::ActionCoordinator` with broad private access
- project-catalog mutation, project or workspace session persistence, command-prompt feedback,
  and menu-surface transitions now use top-level `WorkspaceProjectCatalogCoordinator`,
  `WorkspacePersistenceCoordinator`, `WorkspaceCommandPromptCoordinator`, and
  `WorkspaceMenuCoordinator` types instead of nested shell-owned coordinator classes
- built-in Tree, Search, Problems, and Git sidebar views plus plugin sidebar providers now share
  one `WorkspaceSidebarRegistry*` path for ids, menu wiring, command parsing or completion, and
  project-scoped active-view persistence instead of keeping plugin sidebars as a special-case
  shell path
- sidebar surface state now treats the stable active `view_id` as the source of truth and derives
  built-in versus plugin behavior through the sidebar registry instead of duplicating both enum
  mode and view id on `WorkspaceShell`
- sidebar enum and state models now live in dedicated `WorkspaceSidebarState*` ownership instead
  of staying defined as nested `WorkspaceShell` types
- menu-bar, anchored-menu, and tree-context-menu state transitions now run through a dedicated
  menu coordinator instead of keeping those flows embedded directly on `WorkspaceShell`
- menu-bar, anchored-menu, and tree-context-menu popup state now lives in a dedicated
  `MenuSurfaceState` on the shell instead of staying flattened into the generic `SurfaceState`
- SDL keydown dispatch and per-surface keyboard handling now run through a dedicated key-input
  coordinator, with modal or menu, surface, and editor-domain handling split across dedicated
  `WorkspaceKeyInputCoordinator.cpp`, `WorkspaceKeyInputCoordinatorModal.cpp`,
  `WorkspaceKeyInputCoordinatorSurfaces.cpp`, and `WorkspaceKeyInputCoordinatorEditor.cpp`
  units instead of one monolithic coordinator translation unit
- keydown and text-input coordination now use top-level `WorkspaceKeyInputCoordinator` and
  `WorkspaceTextInputCoordinator` types instead of nested shell-owned coordinator classes
- dirty-save confirmation flow, compare or merge interaction commands, and chrome or editor or
  compare or merge or tab or sidebar or panel mouse routing now use top-level
  `WorkspaceDirtyPromptCoordinator`, `WorkspaceCompareInteractionCoordinator`, and
  `Workspace*MouseCoordinator` types instead of nested shell-owned coordinator classes
- compare-tab open or refresh flow and sidebar mode or refresh or action handling now use
  top-level `WorkspaceDiffTabCoordinator` and `WorkspaceSidebarCoordinator` types instead of
  nested shell-owned coordinator classes
- tab save, reopen, dirty-state, retarget, and project-local dirty-tab enumeration plus
  lifecycle init or shutdown sequencing and prompt-driven path mutation now use top-level
  `WorkspaceTabCoordinator`, `WorkspacePathMutationCoordinator`, and
  `WorkspaceLifecycleCoordinator` types instead of nested shell-owned coordinator classes
- sidebar mode transitions, refresh logic, and git or problem or plugin entry actions now run
  through a dedicated sidebar coordinator split across `WorkspaceSidebarCoordinator.cpp`,
  `WorkspaceSidebarCoordinatorRefresh.cpp`, and `WorkspaceSidebarCoordinatorActions.cpp`
  instead of one monolithic coordinator translation unit
- sidebar mouse input now routes through per-surface button handlers plus dedicated scroll logic
  in `WorkspaceSidebarMouseCoordinator.cpp` and
  `WorkspaceSidebarMouseCoordinatorScroll.cpp` instead of one catch-all mouse coordinator file
- top-level shell mouse routing now splits button-up or button-down handling from motion or wheel
  handling across `WorkspaceShellMouse.cpp` and `WorkspaceShellMouseMotion.cpp` instead of one
  catch-all shell mouse unit
- workspace persistence now splits config/theme state, per-project session state, and
  workspace-session state across `WorkspacePersistenceCoordinator.cpp`,
  `WorkspacePersistenceCoordinatorConfig.cpp`,
  `WorkspacePersistenceCoordinatorSession.cpp`, and
  `WorkspacePersistenceCoordinatorWorkspaceSession.cpp` instead of one large coordinator file
- overlay lifecycle, project-search sidebar flow, and buffer-search actions now live across
  `WorkspaceShellOverlay.cpp`, `WorkspaceShellProjectSearch.cpp`, and
  `WorkspaceShellBufferSearch.cpp` instead of one catch-all overlay/search file
- text composition, typed-input routing, and terminal text entry now run through a dedicated
  text-input coordinator instead of keeping those flows embedded directly on `WorkspaceShell`
- shell redraw invalidation, caret timing, clipboard or text-input-surface routing,
  cursor or hit-testing, and breadcrumb or project-tab presentation now live in dedicated
  `WorkspaceShellRedraw.cpp`, `WorkspaceShellInteraction.cpp`,
  `WorkspaceShellCursor.cpp`, and `WorkspaceShellPresentation.cpp` units instead of staying
  bundled into `WorkspaceShell.cpp`
- editor tab activation or restore flow, close or reload lifecycle, and split-tree or pane-layout
  logic now live across `WorkspaceShellEditor.cpp` and
  `WorkspaceShellEditorSplits.cpp` instead of one catch-all editor translation unit
- project state capture or restore, project-root initialization, and native project-picker flow
  now live across `WorkspaceShellProjects.cpp`, `WorkspaceProjectStateCoordinator.cpp`, and
  `WorkspaceProjectDialogCoordinator.cpp` instead of one catch-all project translation unit
- the active workspace now reuses the same `ProjectWorkspaceState` container shape as project
  catalog entries, so project switching and persistence no longer hand-maintain duplicated move or
  reset lists for tabs, tree or index state, terminals, overlays, diagnostics, command history,
  colorscheme, or editor preferences
- project-tab, compare-or-merge-tab, terminal-tab, project-workspace, prompt, menu, and
  interaction state models now live in dedicated `Workspace*State.h` headers instead of staying
  nested inside `WorkspaceShell.h`; ownership migration is still incomplete, but the shell no
  longer defines those models inline
- `WorkspaceContext` now owns the project catalog, active project state, prompt state, menu state,
  and transient interaction state, with `WorkspaceShell` rebinding its legacy aliases onto that
  context while the remaining controller migrations continue
- project-catalog mutation is the first controller path moved off `WorkspaceShell&`: the
  top-level `WorkspaceProjectCatalogCoordinator` now depends on `WorkspaceContext` plus explicit
  shell callbacks for project activation, persistence saves, plugin-host shutdown, redraw, and
  welcome-state fallback instead of reaching into shell-private fields directly
- lifecycle init, shutdown, quit-request handling, wake-event registration, and cursor teardown now
  run through a `WorkspaceLifecycleCoordinator` that depends on `WorkspaceContext`, a quit flag,
  and explicit lifecycle callbacks instead of `WorkspaceShell&`
- project-local config, session, and workspace-session persistence now run through a
  `WorkspacePersistenceCoordinator` that depends on `WorkspaceContext`, theme or ui-scale state,
  and explicit callbacks for editor preference application, compare or merge tab reconstruction,
  project-root resolution, and project-catalog restoration instead of `WorkspaceShell&`
- dirty-save confirmation now runs through a `WorkspaceDirtyPromptCoordinator` that depends on
  `WorkspaceContext`, the quit-request flag, and explicit callbacks for path-mutation resolution,
  tab saves, project switching, and tab or project closure instead of `WorkspaceShell&`
- menu-bar, anchored-menu, submenu, and tree-context-menu transitions now run through a
  `WorkspaceMenuCoordinator` that depends on `MenuSurfaceState` plus explicit callbacks for menu
  item resolution, popup geometry, action dispatch, and chrome redraw instead of
  `WorkspaceShell&`
- command-prompt input, history, completion, and command execution now run through a
  `WorkspaceCommandPromptCoordinator` that depends on project command state plus explicit
  callbacks for action dispatch, plugin command execution, sidebar-view enumeration, and bottom-panel redraw instead of `WorkspaceShell&`
- compare-tab reuse or merge-tab reuse plus working-tree, branch-head, and conflict-open flows now
  run through a `WorkspaceDiffTabCoordinator` that depends on project tab or overlay state plus
  explicit callbacks for compare or merge tab construction, active-tab synchronization, overlay
  dismissal, and redraw instead of `WorkspaceShell&`
- compare-picker, compare selection, compare scrolling, merge selection, merge scrolling, and
  merge-choice application now run through a `WorkspaceCompareInteractionCoordinator` that depends
  on project compare-picker or compare-tab or merge-tab state plus explicit callbacks for overlay,
  compare, merge, editor-open, and redraw behavior instead of `WorkspaceShell&`
- prompt-driven create, rename, delete, dirty-path resolution, tab retargeting, and
  diagnostic-refresh flows now run through a `WorkspacePathMutationCoordinator` that depends on
  workspace prompt state plus project tab or diagnostics state and explicit callbacks for prompt
  dismissal, editor-tab helpers, compare or merge tab rebuilds, and redraw instead of
  `WorkspaceShell&`
- sidebar mode, refresh, and git or problem or plugin entry actions now run through a
  `WorkspaceSidebarCoordinator` that depends on project workspace state plus explicit callbacks
  for project-open, search, compare, prompt, and redraw behavior instead of `WorkspaceShell&`
- keydown and text-input routing now run through `WorkspaceKeyInputCoordinator` and
  `WorkspaceTextInputCoordinator` that depend on project or prompt or menu or text-input state
  plus explicit callbacks for action dispatch, menu transitions, command prompt, compare or merge
  editing, terminal I/O, and redraw behavior instead of `WorkspaceShell&`
- chrome, sidebar, and panel mouse routing now run through `WorkspaceChromeMouseCoordinator`,
  `WorkspaceSidebarMouseCoordinator`, and `WorkspacePanelMouseCoordinator` that depend on
  project or menu or interaction state plus explicit callbacks for menus, overlay hit-testing,
  terminal selection, tree context menus, and redraw behavior instead of `WorkspaceShell&`
- action-context dispatch, tab save or reopen or retarget flow, and editor or compare or merge
  or tab-strip mouse routing now run through `WorkspaceActionContext`, `WorkspaceTabCoordinator`,
  `WorkspaceEditorMouseCoordinator`, `WorkspaceCompareMouseCoordinator`,
  `WorkspaceMergeMouseCoordinator`, and `WorkspaceTabMouseCoordinator` with explicit state plus
  callback dependencies instead of `WorkspaceShell&`
- production `WorkspaceShell` friend-class access is now gone; only the
  `MICROIDE_TESTING`-guarded `WorkspaceShellTestAccess` friend remains for test fixtures
- the active shell now aliases the `ProjectSurfaceState` stored in the current
  `ProjectWorkspaceState`, and project-scoped sidebar, overlay, and panel state now live in
  dedicated `SidebarState`, `OverlayState`, and `PanelState` models instead of one generic
  surface bag, so project switching no longer hand-copy duplicated sidebar, overlay,
  command-prompt, focus, width, height, or scroll fields between active and persisted UI state
  models
- native project-picker launch, pending-result, and callback bookkeeping now live in a dedicated
  `WorkspaceProjectDialogState` model instead of more flattened dialog state on `WorkspaceShell`
- shell-global text-input surface and composition state now live in a dedicated
  `WorkspaceTextInputState` on `WorkspaceContext`, so IME and typed-input routing no longer keep
  that state flattened directly on `WorkspaceShell`
- transient drag, mouse-selection, and window-focus interaction state now lives outside
  `ProjectSurfaceState`, so project switches clear in-flight gestures instead of leaking stale
  interaction state across projects
- editor blame, diagnostic, or plugin hover targeting now lives apart from popup layout or hover
  lifetime across `WorkspaceShellHoverTargets.cpp` and `WorkspaceShellHoverPopup.cpp` instead of
  one catch-all hover translation unit
- terminal tab open or close, focus-event sync, wake-event consumption, and exited-tab reaping
  now live across `WorkspaceShellTerminal.cpp` and `WorkspaceShellTerminalTabs.cpp` instead of
  one catch-all terminal translation unit
- compare-tab rebuild, compare viewport sync, merge conflict tracking, and merge-tab rebuild now
  live across `WorkspaceShellCompare.cpp` and `WorkspaceShellMergeState.cpp` instead of one mixed
  compare-or-merge state translation unit
- dirty-path detection or save resolution and rename-or-delete tab retargeting now live across
  `WorkspacePathMutationCoordinator.cpp`, `WorkspacePathMutationCoordinatorDirty.cpp`, and
  `WorkspacePathMutationCoordinatorTabs.cpp` instead of one catch-all path-mutation coordinator
- top-level render orchestration now runs through explicit frame, active-surface, window-chrome,
  sidebar, overlay, bottom-panel, menu, prompt, and text-input phase methods backed by dedicated
  `WorkspaceShellRenderFrame.cpp`, `WorkspaceShellRenderChrome.cpp`,
  `WorkspaceShellRenderSidebar.cpp`, `WorkspaceShellRenderOverlay.cpp`,
  `WorkspaceShellRenderBottomPanel.cpp`, `WorkspaceShellRenderMenus.cpp`,
  `WorkspaceShellRenderPrompts.cpp`, and `WorkspaceShellRenderTextInput.cpp` units instead of one
  monolithic `WorkspaceShellRender.cpp`
- repo-owned dogfood plugins now cover a save-driven ESLint diagnostics flow and a small project-local bookmarks sidebar

Open work:

- keep plugin APIs narrow and host-owned; never expose `WorkspaceShell` wholesale
- Phase 1 host-service extraction from `docs/vscode-extension-compatibility-plan.md` is complete;
  Phase 2 contribution and override seams are now active, with unified sidebar view registration
  landed as the first slice rather than more plugin runtime plumbing; the remaining Phase 1
  follow-up work is now mostly shell-ownership shaping and opportunistic debt cleanup rather than
  missing core services
- validate broader native file-watch coverage beyond the current Linux-backed asset watcher once
  target-host build and runtime validation are available; until then snapshot fallback remains
  the correct baseline for macOS and Windows hosts
- extend the new sidebar contribution seam with view ordering, hide or disable controls, built-in
  overrides, and additional host-owned view-container models where plugin pressure justifies it
- continue moving remaining hardcoded accelerators, menu wiring, and extension points behind
  stable registries where plugin pressure justifies it
- add async or background plugin task surfaces only if real plugin workloads require them
- extend the same host-managed runtime-asset model to colorschemes or other non-code assets only if real plugins justify it
- preserve the rule that editing, compare, merge, search, git, and terminal remain built-in product features even when plugins can extend around them

### 2. Terminal Hardening

Current state:

- the embedded terminal is useful and already covers the important full-screen and shell workflows we have exercised so far

Open work:

- broaden real-world validation with actual terminal programs instead of extending escape coverage from guesswork
- fill the remaining ANSI or control-sequence gaps only where real usage justifies them
- keep resize, redraw, scrollback, and wake-event behavior robust under long-running output

### 3. Editor Correctness And Scale

Current state:

- the editor is functionally strong, but the text model is still byte-oriented
- large-file mode, blame shadow text, and retained redraw are shipped
- undo and redo now store line-range patches plus cursor and scroll state instead of full
  document snapshots, which removes the worst buffer-history memory blow-up on large files
- file open and save now run through the shared text-file helper, so the viewport no longer does
  its own stream-buffer file I/O

Open work:

- continue UTF-8 and IME hardening while the underlying text storage is still byte-based
- validate large-file thresholds on larger repositories and adjust only from measured behavior
- validate blame shadow text on real repositories and keep it asynchronous, viewport-scoped, and cheap enough to preserve typing and scrolling latency
- expand compare and merge workflow coverage where editor-side regressions are still too easy to miss

### 4. Project And Git Service Hardening

Current state:

- search is already behind a built-in service boundary
- file operations, blame, compare, and git state already have service seams
- search and blame now share a cancellable background task executor instead of bespoke worker-thread ownership
- plugin and syntax asset reload now flow through a host-owned tree watcher instead of only a
  manual reload command
- subprocess fd ownership is now RAII-backed inside `platform/Subprocess.*` instead of depending
  on repeated manual close paths

Open work:

- keep external tool usage behind `src/project/*` service boundaries
- keep tightening subprocess error reporting and higher-level git command behavior around the
  system `git` path
- move avoidable filesystem and git refresh work off latency-sensitive UI paths
- keep new plugin-facing capabilities layered on structured services rather than letting UI code or plugin glue parse command output directly

### 5. Testing And Performance Discipline

Current state:

- retained redraw has comparison coverage against clean full redraws
- search, tab ordering, context-copy flows, and many workspace mutations already have direct regression tests

Open work:

- add regression tests whenever a bug is fixed; do not rely on “should be covered already”
- keep retained redraw comparison tests serial under SDL dummy video because they share global SDL state
- keep profiling startup, redraw, typing, scrolling, and idle behavior with the tracing docs in this directory
- preserve the current redraw architecture unless profiling shows a new hotspot; the remaining work is policy tuning and regression coverage, not a wholesale redraw rewrite
- prefer targeted app-level burst tests only when shell-level retained-redraw tests stop catching the right bugs

## Deferred Or Out Of Scope

These are not current project work unless deliberately promoted into their own phase:

- debugging
- plugin marketplaces, remote install flows, and Micro-plugin compatibility
- cloud or collaboration features
- built-in AI or chat surfaces
- recent-project and recent-file affordances
- soft wrap
- diagnostics as an implicit requirement; diagnostics only if a dedicated diagnostics phase is started

## Companion Docs

Keep these when you need deeper design context:

- `AGENTS.md`: repo-level engineering policy, iteration loop, and agent expectations
- `docs/implementation-guide.md`: durable product direction
- `docs/plugin-runtime-research.md`: deeper plugin architecture notes and external references
- `docs/diff-editor-merge-rewrite-plan.md`: subsystem-specific rewrite plan for diff and merge presentation
- `docs/production-tech-debt-review.md`: structural debt worth paying down as large phases proceed
- `docs/performance-findings.md`: concrete shipped performance wins worth preserving
- `docs/startup-tracing.md`: startup profiling workflow
- `docs/runtime-profiling.md`: runtime and redraw profiling workflow
