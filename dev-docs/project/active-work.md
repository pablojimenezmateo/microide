# MicroIDE Active Work

Reviewed on 2026-06-29 (v2.4.0 shipped — performance & correctness cycle: allocation-free
editor/render/terminal hot paths, piece-tree document model, GPU-gated glyph atlas, debugger
surfaced as first-class, plus a `--version` CLI flag. Prior: v2.3.0 plugin rendering surface).

This is the single source of truth for:

- the shipped baseline that matters for ongoing work
- the current priority stack
- accepted scope cuts and deferred work

Current validation emphasis is still the native diff/merge/git workstation flow:
open repo -> inspect changes -> diff files -> resolve merge conflict -> stage/commit.

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
- shell cards, compact tooltips, framed text inputs, buttons, selectable-list backgrounds,
  popup-menu rows, strip tabs, and common shell glyphs now route through shared workspace render
  primitives instead of staying fully surface-local
- project-local workspace state plus app-level restore of open project tabs
- normal editor tabs, compare tabs, and merge tabs, with deferred-commit tab drag (ghost preview) consistent across all three tab types
- first-class editor groups (`EditorGroup`): split right/down from tab and tree context menus, per-group layout/render/tab-strips, group-aware keyboard input, split/focus/close commands, and session persistence/restore (this supersedes the older nested in-tab shared-buffer split model, collapsed in v2.1.0)
- searchable command palette overlay (Ctrl+Shift+P) that doubles as the command line: typing a verb plus arguments (e.g. `colorscheme dark`) or any unmatched query runs through the shared command executor, and Tab completes command/path tokens. This is now the *only* command surface — the separate Ctrl+E bottom-panel command prompt (and per-session command history) was retired and folded in
- recent projects/files (MRU) surfaced in the file finder, and a state-aware welcome/home surface: a cold-start variant (Open Folder + recent projects) when no project is open, and a project-home variant (project name, recent files in the project, New File / Open File / Find in Project) when a project is open with no editor tab. Plus a built-in light theme
- decorated-row assembly is unified across editor, compare, and merge surfaces (shared intra-line underline and conflict-marker helpers), keeping the three diff/merge surfaces convergent
- plugin rendering surface (shipped v2.3.0): plugins run on a dedicated worker thread and contribute editor decorations, content surfaces, ghost text, host-owned buffer edits, reactive editor events, language providers, presentation contributions (themes/file-icons/status), and tree sidebars under a strict host-renders-data model — validated zero-cost when unused
- release artifacts are GPG-signed: the published `.deb` carries a detached `.asc` signature and SHA256 checksum, the public key ships as `microide-signing-key.asc`, and release tags are signed (see `dev-docs/project/release-checklist.md`)
- multi-caret editing with position remap across edits, region-stack highlighting, and copy-with-context
- editor open/save/reopen, selection, clipboard, undo/redo, line numbers, **word wrap (soft-wrap)** with wrap-aware caret motion and hit-testing, horizontal scrolling when wrap is off, dirty tracking, IME hooks, and project-local preferences (this supersedes any older roadmap note that listed soft wrap as out of scope)
- syntax-highlight state now uses coarse document checkpoints plus per-line memoized replay, so
  far jumps in large files do not have to rebuild highlight state from file start
- UTF-8 boundary logic, line-ending decode or serialize, and text splitting now route through one
  shared `util/StringUtil.*` layer across viewport, renderer, terminal, and workspace helpers
- single-line shell text inputs now share one insertion, caret, composition, and tail-truncation path across prompts, command input, overlays, and sidebar search fields, while read-only viewport-backed text surfaces still participate in shared selection and copy actions
- editor undo and redo now store changed line ranges plus view state instead of full-buffer snapshots, and editor file open/save now reuses the shared text-file helper instead of inline stream assembly
- document saves are durable (temp-file `fsync` via shared `util/DurableFile`, matching the persisted-state writer) and guarded against silent clobbering: each open buffer records an on-disk signature (mtime+size) at load/save, and a save whose file changed underneath it is refused and surfaced as a non-blocking external-change banner (Reload / Overwrite / Keep) instead of the old blocking modal; clean buffers reload silently with a passive "reloaded from disk" notice, and the watcher's echo of the editor's own write is suppressed by signature
- filesystem tree with `.gitignore` handling, git markers (async after first paint on project
  open), refresh, and trash-backed create/rename/delete flows
- host-owned app-directory, trash or recycle-bin, open-URL, reveal-path, and bundled-asset
  services for Linux, macOS, and Windows policy, with runtime assets copied into desktop-build
  and macOS-bundle layouts
- file finder overlay plus async project search with literal or regex mode, case controls, hidden-file controls, replace-in-project for literal mode, capped-result feedback, and a standalone benchmark tool
- git sidebar with compare, merge, stage, unstage, discard, outgoing-file views, bulk stage-all, and confirmed discard-all
- PTY-backed terminal tabs with scrollback, selection, copy/paste, alternate screen, title updates, OSC 52 clipboard copy, focus notifications, bracketed paste, cursor-key mode, origin mode, autowrap control, and the common ANSI scroll-region paths currently needed by real tools
- runtime syntax highlighting from the in-tree generated syntax snapshot plus plugin `syntax/*.lua` contributions loaded into the host tokenizer at startup and `plugins-reload`
- manual Lua plugin loading from the user config directory only, lifecycle hooks, plugin commands, plugin sidebars, project-relative file helpers, active-buffer metadata, argv-based process helpers, repo-owned dogfood plugins (installed under user config), and `plugins-reload`
- plugin-published diagnostics with host-owned storage, theme-backed underline rendering, severity gutter markers, host-owned blame/diagnostic/plugin hover popups in editor surfaces, plugin hover providers, and host rename/delete cleanup for stale diagnostic paths
- targeted regression coverage across compare, merge, git services, file operations, retained redraw, workspace chrome, and plugin-adjacent registries

## Active Phases

The chronological log of shipped-and-archived changes that used to live here has been removed — that
history is recorded faithfully in `CHANGELOG.md` (user-facing) and `openspec/changes/archive/`
(per-change proposal/spec/tasks). The durable scope decisions those entries carried (hosted CI and a
self-hosted perf-runner are descoped; non-Linux host backends are not being built) are recorded under
**Deferred Or Out Of Scope** below. This section now describes only what is genuinely active.

### 0. Debugger / DAP Support — shipped in v2.0.0 (2026-06-20)

Full interactive debugging via the Debug Adapter Protocol, host-owned (mirroring the
LSP client), behind a master "Enable debugger" toggle. **Phases 0–10 are complete and
shipped in the v2.0.0 release**: protocol client core, session lifecycle, breakpoints
(conditional / hit-count / logpoint / function / exception filters), execution control
(including capability-gated reverse execution), Variables/Scopes, hover-to-inspect,
watches, multi-session debugging, the right-side debug pane, and console REPL — with a
bundled `gdb-dap` plugin for gdb 17.2 and an external control channel for headless /
agent-driven operation. The full roadmap and status live in
`dev-docs/debugger/dap-integration.md`; future hardening and additional adapters continue
from there.

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
- `WorkspaceActionCoordinator` now executes from a value `WorkspaceActionContext` instead of
  taking `WorkspaceShell&`
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
  and transient interaction state, and `WorkspaceShell` now reaches that state directly instead of
  keeping its old active-project reference-alias member block
- project-catalog mutation is the first controller path moved off `WorkspaceShell&`: the
  top-level `WorkspaceProjectCatalogCoordinator` now depends on `WorkspaceContext` plus explicit
  shell callbacks for project activation, persistence saves, plugin-host shutdown, redraw, and
  welcome-state fallback instead of reaching into shell-private fields directly
- completion, snippet-session, code-action, go-to-definition, and find-references coordination now
  route through `AssistService`; shell action, key-input, overlay, text-input, and test-access
  call sites now bind that service directly instead of keeping a shell-specific assist facade
  (`AssistService::Operations` remains a transitional seam and should shrink into smaller explicit
  assist ports over time, not grow into a shell callback bag)
- bottom-panel tab-strip geometry queries now bind `TabStripService` directly from render,
  cursor, tab-mouse, and test-access paths instead of routing through shell-owned
  `ComputeVisibleBottomPanelTabs` / `ComputeVisibleTerminalTabs` wrappers
- `TextViewport` now has a dedicated `TextViewportViewState.cpp` translation unit for viewport
  sizing, scroll clamping, wrap/fold toggles, cursor movement, wrapped-row cursor mapping, and
  caret-advance helpers; the next meaningful refactor seam is undo/history ownership, not more
  helper sharding
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
- `WorkspaceShell` still acts as the app-facing facade, but the shell-breakdown plan is now
  implemented: event routing, wake routing, action enablement, render composition, and test hooks
  all live behind explicit seams instead of direct shell-owned monoliths
- worker-thread execution + rendering surface (on `feat/plugin-rendering`): all `lua_State` touches
  now run on a dedicated plugin worker thread with a UI-owned snapshot/mailbox boundary, and plugins
  gained a host-renders-data presentation surface (`ctx.decorations`, `ctx.surface`,
  `ctx.editor.apply_edits`, `ctx.editor.set_ghost_text`) plus async language queries and reactive
  editor events. The old detached async-process subsystem was deleted; `ctx.process.run_async` now
  runs on the worker
- sandbox re-review (2026-06-26): the widened surface stays sound — render contributions are
  validated, size-capped data the host draws (display-list op/point/text/image caps, 256 MiB
  `SurfaceTextureCache` budget, `stb_image` decode bounded to 64 MiB / 8192² and fuzzed); capability
  containment, path checks, and the per-call watchdog are intact. One correctness gap was found and
  fixed: the `ctx.process.run_async` completion callback inherited the enclosing call's already-spent
  750ms watchdog deadline (so a subprocess outlasting the budget got its healthy callback aborted on
  the first instruction); it now runs under `LuaRuntime::PCallNested` with a fresh deadline. Authoritative
  detail lives in `guidelines/plugin-trust-model.md`

### 2. Cross-Platform Host Support

Current state:

- app-directory policy, trash or recycle-bin behavior, external URL handling, and runtime asset
  discovery now route through dedicated `src/platform/*` services instead of Linux-first callers
- sync and async subprocess execution now route through a dedicated `platform/ProcessBackend`
  service seam instead of embedding POSIX process launch directly in the public facades
- terminal lifecycle now routes through `platform/TerminalBackend`, so `TerminalSession` keeps the
  screen model and escape handling while host launch/resize/shutdown behavior sits behind a host
  service boundary
- CMake now supports macOS bundle output, Windows desktop output, and non-`pkg-config` package
  discovery for PCRE2
- local bring-up now exists for Linux, macOS, and Windows host-facing build or test paths

Descoped (2026-06-17): Linux is the supported host. The `src/platform/*` seams
(`ProcessBackend`, `TerminalBackend`, `FileIndexWatcher`, app-directory / trash / URL services)
stay in place because they keep the host boundary clean and testable, but native Windows and
macOS subprocess, terminal, and file-watcher backends are **no longer a project objective**. On
non-Linux hosts the poll/snapshot watcher fallback is the accepted permanent baseline, and
host-platform CI is not being pursued. See **Deferred Or Out Of Scope**.

Additional current state (shell decomposition, recorded here historically):

- chrome, sidebar, and panel mouse routing now run through `WorkspaceChromeMouseCoordinator`,
  `WorkspaceSidebarMouseCoordinator`, and `WorkspacePanelMouseCoordinator` that depend on
  project or menu or interaction state plus explicit callbacks for menus, overlay hit-testing,
  terminal selection, tree context menus, and redraw behavior instead of `WorkspaceShell&`
- action-context dispatch, tab save or reopen or retarget flow, and editor or compare or merge
  or tab-strip mouse routing now run through `WorkspaceActionContext`, `WorkspaceTabCoordinator`,
  `WorkspaceEditorMouseCoordinator`, `WorkspaceCompareMouseCoordinator`,
  `WorkspaceMergeMouseCoordinator`, and `WorkspaceTabMouseCoordinator` with explicit state plus
  callback dependencies instead of `WorkspaceShell&`
- production `WorkspaceShell` friend-class access is now gone, and the old
  `WorkspaceShellTestAccess` friend path is gone too; shell tests now use the public
  `MICROIDE_TESTING`-gated `WorkspaceShell::TestAccess` API from
  `workspace/WorkspaceShellTesting.h`
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
- top-level `ActionId` enablement now runs through a dedicated `WorkspaceActionAvailability`
  helper backed by `WorkspaceContext` and read-only editor or compare or terminal callbacks instead
  of `WorkspaceShell::IsActionEnabled`
- top-level event routing now runs through `WorkspaceEventDispatcher`, scheduled wake handling now
  runs through `WorkspaceWakeController`, and the shell's action, render, and event seams are
  wired by a dedicated `WorkspaceShell::Bootstrapper` composition root
- `WorkspaceShell` render entry points now delegate the ordered frame composition path to
  `WorkspaceRootView`, which now composes dedicated active-surface, chrome, sidebar, overlay,
  panel, menu, and prompt views instead of acting as a single minimal placeholder seam
- repo-owned dogfood plugins now cover a session-scoped ESLint diagnostics flow plus formatter and LSP integrations

- Phase 2 contribution and override model is now shipped:
  - `WorkspaceKeybindingRegistry*` defines named built-in keybinding specs with stable IDs,
    context awareness (global / editor / sidebar / terminal), and `ResolveKeybindings` that merges
    built-ins with plugin contributions and applies user disable overrides
  - `WorkspaceSettingsRegistry*` defines built-in setting specs with typed defaults (bool, int,
    float, string, enum), and merges with plugin-declared settings via `AllSettingInfos`
  - `WorkspaceStatusRegistry*` resolves plugin-contributed status bar items sorted by alignment
    and priority
  - `WorkspaceMenuRegistry*` exposes `ContributedMenuItems` to surface plugin menu entries for
    any `MenuId`, alongside the static built-in menu specs
  - `WorkspaceSidebarRegistry*` now exposes `OrderedSidebarViews` and `SidebarViewPolicy` so
    the host can hide or reorder sidebar views based on user-persisted policy
  - `PluginHost` exposes four new Lua tables: `ctx.settings` (declare / get), `ctx.menus` (add),
    `ctx.keybindings` (add), and `ctx.status` (add / update); corresponding C++ accessors
    `ContributedSettings`, `ContributedMenuEntries`, `ContributedKeybindings`, and
    `ContributedStatusItems` plus `UpdateStatusItem` are available to workspace coordinators
  - `PluginHost::Callbacks` gains `get_setting` and `request_status_redraw` for the workspace
    layer to supply setting values and receive status-update redraw signals
  - `PersistedUserConfigState` now carries `settings` (id→value pairs) and
    `disabled_keybinding_ids`; `PersistedProjectConfigState` now carries `settings` and
    `sidebar_policies`; all fields round-trip through the existing line-based serialisation format
  - `WorkspaceKeyInputCoordinator` now resolves contributed and user-disabled keybindings at
    runtime instead of using a hardcoded shortcut path
  - `WorkspaceMenuCoordinator` and the shell chrome now surface contributed menu items,
    ordered sidebar views, and compact status items on the live UI path
  - persistence and plugin callback wiring now restore setting values, sidebar policy, and
    status redraw behavior end to end
  - all new registries have full test coverage in `tests/ContributionRegistryTests.cpp`

Open work:

- keep plugin APIs narrow and host-owned; never expose `WorkspaceShell` wholesale
- the Linux-backed asset watcher is the supported baseline; broader native file-watch coverage on
  non-Linux hosts is descoped (see **Deferred Or Out Of Scope**), and the snapshot fallback is the
  accepted permanent baseline elsewhere
- add async or background plugin task surfaces only if real plugin workloads require them
- preserve the rule that editing, compare, merge, search, git, and terminal remain built-in
  product features even when plugins can extend around them

- Phase 3 async service platform is now shipped:
  - `platform/AsyncSubprocess.*` provides a POSIX-backed async subprocess interface with
    bidirectional pipes, poll-based non-blocking reads, and SIGTERM/SIGKILL shutdown
  - `util/JsonValue.*` implements a recursive JSON parser and serializer for LSP communication
  - `workspace/WorkspaceLspClient.*` implements an async JSON-RPC 2.0 client with initialize,
    didOpen, didChange, didSave, didClose notifications and textDocument/hover,
    textDocument/completion, textDocument/codeAction, textDocument/formatting,
    textDocument/definition, and textDocument/references requests delivered back to the host
    through SDL wake events
  - `workspace/WorkspaceLspManager.*` manages multiple LSP servers, one per language_id
  - `workspace/WorkspaceFormatterRegistry.*` stores declarative formatter specs (language_id,
    command, label)
  - `workspace/WorkspaceSaveParticipants.*` stores save-participant specs for Lua callbacks
  - `workspace/WorkspaceCompletionRegistry.*` stores language-specific completion-provider specs
  - `workspace/WorkspaceCodeActionRegistry.*` stores language-specific code-action-provider specs
  - `workspace/WorkspaceTaskRegistry.*` stores runnable task specs with subprocess command,
    label, group, and working directory
  - `workspace/WorkspaceToolRegistry.*` stores downloadable tool specs with platform,
    download URL, and SHA256 checksum
  - `workspace/WorkspaceToolDownloader.*` provides caching and download orchestration for tools
  - `workspace/WorkspaceTestController.*` manages test discovery and execution results
  - `workspace/WorkspaceOutputChannels.*` (already shipped from Phase 2) provides named log
    channels for tool output
  - `PluginHost` gains seven new Lua tables: `ctx.formatters` (add), `ctx.save_participants` (add),
    `ctx.completion` (add), `ctx.code_actions` (add), `ctx.tasks` (add), `ctx.tools` (add),
    `ctx.tests` (add); corresponding C++ accessors are available to workspace coordinators
  - `PluginHost` now exposes runtime query or execution paths for save participants, completion
    providers, code-action providers, and test providers
  - `workspace/WorkspaceLspClient.*` now supports `textDocument/formatting`
  - the editor save path now runs save participants before formatter execution and writes the
    transformed buffer back into the viewport before disk save
  - `WorkspaceToolDownloader::Download(...)` is no longer a stub and now validates cached or
    local file installs
  - built-in commands, menus, and keybindings now surface completion, code actions, tasks, test
    discovery or execution, and output channels through live shell state
  - completion and code actions now render through dedicated host-owned editor overlays, while
    task and test flows reuse the bottom panel and Tests sidebar instead of inventing parallel UI
  - runtime and shell wiring are covered in `tests/PluginHostTests.cpp` and
    `tests/WorkspaceShellPluginTests.cpp`, in addition to `tests/Phase3Tests.cpp`
  - Phase 5 validation now exercises plugin-declared language servers end to end for diagnostics,
    completion, code actions, go-to-definition, references, and editable merge-buffer lifecycle
    in `tests/Phase5Tests.cpp`

Open work:

- keep validating real LSP server communication beyond the shipped end-to-end fake-server
  coverage before promising broader language-server coverage
- keep the completion and code-action overlays host-owned and minimal; do not fork the command
  prompt into a second editor interaction model
- extend test UX only after real controller state exists for richer tree, gutter, and per-test
  run workflows
- add remote tool-download transports only when a real workflow needs them; the shipped path is
  cache-backed local install plus SHA validation

- Phase 4 SCM, review, and advanced provider surfaces now shipped:
  - `workspace/WorkspaceScmRegistry.*` manages source control provider registrations
  - `workspace/WorkspaceAnnotationRegistry.*` manages blame, decoration, and margin annotation
    providers per language
  - `workspace/WorkspaceVirtualDocument.*` provides virtual document support for diff views,
    merge views, and generated content
  - `workspace/WorkspaceReviewComments.*` manages inline code review comments and discussion
    threads with state tracking
  - the authentication-provider and host-managed secret-storage surfaces (`WorkspaceAuthProvider.*`,
    `WorkspaceSecretStorage.*`) have been retired alongside the Phase 5 AI scope
  - `PluginHost` gains two new Lua tables: `ctx.scm` (add) and `ctx.annotations` (add); virtual
    documents and review comments are host-managed
  - `WorkspaceShell` rebuilds SCM and annotation registries from plugin contributions on
    reload, and virtual documents now open and refresh in the live tab model
  - the Git sidebar now shows SCM summary lines and review comments render as gutter markers
    in editor and virtual-document views
  - direct coverage now exists in `tests/WorkspaceShellPluginTests.cpp` and
    `tests/Phase4Tests.cpp`

Open work:

- keep the built-in Git compare and stage flows host-owned until a cohesive provider-driven source
  control design is ready
- extend review UX from gutter markers to richer thread panels, compose, edit, and resolve only
  after location mapping and persistence rules are stable
- validate GitLens-like and GitHub-review-like workflows against the current provider seams before
  broadening them

- Phase 5 AI and LLM runtime surfaces are retired from the product scope.

### 6. Deferred Work And Throughput Pass (2026-05-02)

This phase addresses background-thread isolation, event-driven file watching, search throughput,
and adaptive idle rendering. The infrastructure layer **and** the workspace wiring are now shipped
(verified 2026-06-11; the items previously listed under "Open work" all landed).

Shipped:

- `platform/FileIndexWatcher.*` platform abstraction with Linux `inotify`, macOS `FSEvents`,
  Windows `ReadDirectoryChangesW`, and a poll-fallback backend; callback fires on the watcher
  thread with an `IndexUpdateBatch` (created/deleted/renamed entries with path + mtime)
- `project/ProjectBackgroundExecutor.*` single-thread per-project executor with `Post`,
  `PostLatest` (debounce by key), `Cancel`, and `Shutdown(deadline)` for background subprocess
  dispatch
- `project/PatternCache.*` PCRE2 pattern cache with LRU eviction at 64 entries; thread-safe with
  `std::mutex`; eliminates repeated compile + JIT on repeated searches
- `app/BackgroundTaskCounter.*` global atomic counter wired into search dispatch and
  `ProjectBackgroundExecutor`; `IncrementBackgroundTaskCount` / `DecrementBackgroundTaskCountAndWake`
  keep the event loop awake during in-flight background work
- PCRE2 JIT compilation added to `util/RegexUtil.h` immediately after `pcre2_compile`; JIT
  unavailability emits a one-time `SDL_Log` and falls back to interpreted mode
- Architecture lint rules in `tests/ArchitectureInvariantsTests.cpp`:
  `CheckNoSynchronousSubprocessWaitInWorkspace` (hard-fail on `Subprocess::Wait` / `waitpid` /
  `WaitForSingleObject` in workspace TUs) and `CheckLspDidOpenIsNonBlocking` (policy rule)
- Perf fixture: 10 000-file flat project under `tests/perf/fixtures/file_finder_large/`
- Perf fixture: pre-seeded 1 000-file git repository under `tests/perf/fixtures/git_status_project/`
- Perf baselines committed: `file_finder_cold.json`, `git_sidebar_activate.json`,
  `search_first_result.json` under `tests/perf/baselines/`; `idle_soak_30s` scenario extended with
  wakeup-rate assertion
- Full unit test coverage for `BackgroundTaskCounter`, `FileIndexWatcher`, and `PatternCache`

Workspace wiring (was "Open work"; all landed and verified in tree on 2026-06-11):

- `FileIndexWatcher` is wired to project open/close in
  `WorkspaceProjectStateCoordinator` (`StartFileIndexWatcherForCurrentProject` /
  `StopFileIndexWatcher`), and the file finder/search consume `state.file_index` snapshots
- git `Status` dispatches through `ProjectBackgroundExecutor::PostLatest`
  (`GitRepositoryService`), blame runs on its own background `TaskExecutor`
  (`GitBlameService`), and sidebar refresh is async with a `refreshing` flag
- `ComputeLayout` is guarded by `layout_dirty_` in `WorkspaceShellRenderFrame.cpp`
  (set via `MarkLayoutDirty()`), and `visible_line_range` is plumbed into `FrameToken`
- incremental search streaming publishes `kBatchSize` batches with progress wake events
  (`ProjectSearchService`); the runtime appends cumulatively
- the `IdleHint`-driven event loop is live in `Application.cpp`
  (`Full` → `SDL_PollEvent`, `CaretOnly` → `SDL_WaitEventTimeout`, `Idle` → `SDL_WaitEvent`),
  fed by `CurrentIdleWaitState()`; the caret-blink delay freezes after
  `kCaretBlinkIdleStopMs`, so a focused-but-idle editor still reaches a full blocking wait

Idle-CPU note (2026-06-11): a proposal to suppress the project file-monitor poll delay
whenever a native watcher is configured was investigated and rejected. On a healthy Linux
host the native inotify backend already sets `polling_required_ = false`, so
`FileTreeWatcher::NextPollDelay()` returns `nullopt` and the loop blocks fully; the poll
delay is only non-null when native arming fails (missing root, unreadable subtree, or
inotify watch exhaustion on very large repos), where polling is the *only* way to detect
changes. Forcing it off there would silently miss filesystem changes — a correctness
regression — so the current behavior is correct as-is.

Follow-up (2026-06-18): the polling fallback above could itself freeze the UI. Opening a
project with a very large, unpruned tree (e.g. tens of thousands of dirs with no root
`.gitignore`) exhausts inotify, drops to polling, and then re-walked the whole tree twice
(`CaptureTreeSnapshot` + `CollectRecursiveWatchPaths`) **on the shell thread** every poll
interval — pinning a core at ~100%. Fixed by: (1) bounding every recursive walk with
`platform::kTreeTraversalEntryBudget` (50k entries) and reporting truncation; (2) when a
tree exceeds the budget, `FileTreeWatcher` enters a "too large" mode where it skips native
arming, never snapshot-diffs a truncated (order-unstable) walk, and returns `nullopt` from
`NextPollDelay()` so periodic polling is suppressed entirely; (3) moving the remaining
polling walks off the shell thread via `WorkspaceProjectFileMonitor::SetBackgroundPoster`
(`ProjectBackgroundExecutor::PostLatest`), delivering results through the existing wake
event; and (4) surfacing a one-time "Project too large for live file watching" notification
(`ConsumeTreeTooLargeNotice`). Net: live watching still works for normal/large-but-pruned
projects (directory `.gitignore` entries are skipped without descending, so they don't count
against the budget); only pathologically large trees degrade to refresh-on-demand.

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
- a 2026-04-23 deep-dive identified several untracked render-path bottlenecks; all nine actionable
  findings from that pass are now confirmed fixed; see `dev-docs/performance/performance-findings.md`
  (Deep-Dive Findings section) for the full record
- a 2026-04-23 second static pass confirmed all previous fixes and found four new bottlenecks;
  the review-comment marker, terminal snapshot generation, editor-pane-layout, and terminal cursor
  lock items from that pass are now fixed;
  see `dev-docs/performance/performance-findings.md` (Second Performance Pass section) and
  `guidelines/tech-debt/archive/2026-05-01-render-and-layout-perf-batch.md` (§8–§11) for the record

Open work:

- add regression tests whenever a bug is fixed; do not rely on “should be covered already”
- keep retained redraw comparison tests serial under SDL dummy video because they share global SDL state
- keep profiling startup, redraw, typing, scrolling, and idle behavior with the tracing docs in
  this directory
- LTO in perf/release builds is acceptable and currently useful, but it is not a substitute for
  profiling render hot paths; if `editor_sticky_scroll_scroll` still regresses, profile the
  residual cost directly and either fix it or explicitly accept it with data
- preserve the current redraw architecture unless profiling shows a new hotspot; the remaining
  work is policy tuning and regression coverage, not a wholesale redraw rewrite
- prefer targeted app-level burst tests only when shell-level retained-redraw tests stop catching
  the right bugs
- the highest-priority remaining follow-ups from the performance passes are:
  1. cold syntax-definition reload: only promote disk caching or parallel plugin syntax parsing if
     profiling shows plugin Lua parsing or plugin regex compilation as material startup cost after
     the generated-registry reuse landed
  2. keep adding focused perf regressions when new hot paths are fixed, especially where cache or
     redraw locality can silently regress without changing user-visible behavior
- measure these with `MICROIDE_PERF_TRACE=1` before and after any fix; do not rely on code
  review alone to confirm impact

## Deferred Or Out Of Scope

These are not current project work unless deliberately promoted into their own phase:

- plugin marketplaces, remote install flows, and Micro-plugin compatibility
- plugin sandboxing, marketplace trust, and project-local plugin loading (ships
  `--disable-plugins` / `--safe-mode` only; see `dev-docs/project/git-workstation.md`)
- cloud or collaboration features
- recent-project and recent-file affordances
- diagnostics as an implicit requirement; diagnostics only if a dedicated diagnostics phase is started
- native Windows/macOS host backends (subprocess, terminal, file-watcher) — Linux is the supported
  host; the `src/platform/*` seams remain but non-Linux backends are not being built, and the
  poll/snapshot watcher fallback is the accepted permanent baseline for non-Linux hosts
- hosted CI and a self-hosted `perf-runner-v1` captured-baseline perf gate — local
  `tools/run-checks.sh` (`tests`/`asan`/`ubsan`/`tsan`) plus the manual fuzz/perf targets are the
  supported validation path

## Git Workstation

The workstation release scope, safe startup flags, release checklist, and trust documentation are
captured in the docs below (the `prepare-git-workstation-preview` OpenSpec change that originally
defined this scope has shipped and is no longer a live change):

- `dev-docs/project/git-workstation.md` — supported / unsupported workflows
- `dev-docs/project/release-checklist.md` — tag, artifacts, tested-workflows matrix
- `SECURITY.md` — trust model, safe mode, reporting

## Companion Docs

Keep these when you need deeper design context:

- `openspec/specs/product-vision/spec.md`: authoritative product thesis and non-goals
- `openspec/specs/diff-merge-editor/spec.md`: durable compare and merge behavioral contract
- `openspec/specs/performance-budgets/spec.md`: durable performance budget policy
- `AGENTS.md`: repo-level engineering policy, iteration loop, and agent expectations
- `dev-docs/project/implementation-guide.md`: durable product direction
- `dev-docs/plugins/plugin-runtime-research.md`: deeper plugin architecture notes and external references
- `dev-docs/project/known-tech-debt.md`: concrete open debt still worth preserving
- `dev-docs/design/text-surface-unification.md`: durable text-input and navigable-text interaction contract
- `dev-docs/performance/performance-findings.md`: concrete shipped performance wins worth preserving
- `dev-docs/performance/startup-tracing.md`: startup profiling workflow
- `dev-docs/performance/runtime-profiling.md`: runtime and redraw profiling workflow
- `dev-docs/project/editor-essentials.md`: shipped editor language contract, folding, presentation toggles, shaping, save normalization

Archived (shipped or superseded):
- `dev-docs/archive/plugin-platform-expansion-plan.md`: plugin platform planning — shipped across Phases 1–5
- `dev-docs/archive/production-tech-debt-review.md`: 2026-04-20 structural debt review — major items resolved by the shell-breakdown pass
- `dev-docs/archive/responsive-shell-layout.md`: ASCII layout reference — contract in `openspec/specs/responsive-shell-layout/spec.md`
- `dev-docs/archive/workspace-shell-testaccess-audit.md`: TestAccess cleanup audit (2026-04-29 pass)
- `dev-docs/archive/vscode-extension-compatibility-plan.md`: explicit out-of-scope decision
