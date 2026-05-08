# MicroIDE Active Work

Reviewed on 2026-05-08.

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
- shell cards, compact tooltips, framed text inputs, buttons, selectable-list backgrounds,
  popup-menu rows, strip tabs, and common shell glyphs now route through shared workspace render
  primitives instead of staying fully surface-local
- project-local workspace state plus app-level restore of open project tabs
- normal editor tabs, compare tabs, merge tabs, and nested shared-buffer splits
- editor open/save/reopen, selection, clipboard, undo/redo, line numbers, soft-wrap rendering with wrap-aware caret motion and hit-testing, horizontal scrolling when wrap is off, dirty tracking, IME hooks, and project-local preferences
- syntax-highlight state now uses coarse document checkpoints plus per-line memoized replay, so
  far jumps in large files do not have to rebuild highlight state from file start
- UTF-8 boundary logic, line-ending decode or serialize, and text splitting now route through one
  shared `util/StringUtil.*` layer across viewport, renderer, terminal, and workspace helpers
- single-line shell text inputs now share one insertion, caret, composition, and tail-truncation path across prompts, command input, overlays, and sidebar search fields, while read-only viewport-backed text surfaces still participate in shared selection and copy actions
- editor undo and redo now store changed line ranges plus view state instead of full-buffer snapshots, and editor file open/save now reuses the shared text-file helper instead of inline stream assembly
- filesystem tree with `.gitignore` handling, git markers, refresh, and trash-backed create/rename/delete flows
- host-owned app-directory, trash or recycle-bin, open-URL, reveal-path, and bundled-asset
  services for Linux, macOS, and Windows policy, with runtime assets copied into desktop-build
  and macOS-bundle layouts
- file finder overlay plus async project search with literal or regex mode, case controls, hidden-file controls, replace-in-project for literal mode, capped-result feedback, and a standalone benchmark tool
- git sidebar with compare, merge, stage, unstage, discard, outgoing-file views, bulk stage-all, and confirmed discard-all
- PTY-backed terminal tabs with scrollback, selection, copy/paste, alternate screen, title updates, OSC 52 clipboard copy, focus notifications, bracketed paste, cursor-key mode, origin mode, autowrap control, and the common ANSI scroll-region paths currently needed by real tools
- runtime syntax highlighting from the in-tree generated syntax snapshot plus plugin `syntax/*.lua` contributions loaded into the host tokenizer at startup and `plugins-reload`
- manual Lua plugin loading from user and project directories, lifecycle hooks, plugin commands, plugin sidebars, project-relative file helpers, active-buffer metadata, argv-based process helpers, repo-owned dogfood plugins, and `plugins-reload`
- plugin-published diagnostics with host-owned storage, theme-backed underline rendering, severity gutter markers, host-owned blame/diagnostic/plugin hover popups in editor surfaces, plugin hover providers, a built-in Problems sidebar, and host rename/delete cleanup for stale diagnostic paths
- targeted regression coverage across compare, merge, git services, file operations, retained redraw, workspace chrome, and plugin-adjacent registries

## Active Phases

Update (2026-05-06): `codebase-cleanup-perf-and-debt` shipped focused cleanup slices across render,
editor, subprocess, and persistence paths: sidebar query/replace fallback text now materializes in
`RenderViewModelBuilder`, clipboard and replace-all flows reduced large transient allocations,
formatter and tool SHA verification dispatch now run through background execution seams, and the
legacy persistence importer was removed in favor of structured-record-only persistence.

Update (2026-05-07): `responsive-layout-and-options-polish` shipped the responsive shell pass:
window scaling now avoids retained-text blur on HiDPI/fractional-scale displays, the menu bar keeps
all top-level menus reachable through compact/overflow chrome, hit pads cover small resize, close,
scrollbar, and terminal-tab controls, the bottom status bar is host-owned and clickable, and
Settings and Help/About overlays now share one settings-overlay service and
view-model-rendered surface.

Update (2026-05-08): `stabilize-ci-and-remove-periodic-workflows` is in progress. Active CI policy
now requires event-driven triggers only (`push`, `pull_request`, `workflow_dispatch`) and removes
periodic schedules. `perf-harness` and `fuzz` route extended coverage to manual dispatch instead of
nightly cron runs. Current remaining blocker is repository billing for GitHub-hosted runners,
which prevents workflow jobs from starting until account payments/spending limits are restored.

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
  discovery for PCRE2 and libcurl
- local bring-up now exists for Linux, macOS, and Windows host-facing build or test paths, while
  CI is intentionally deferred until the cross-platform backends settle

Remaining work:

- implement Windows subprocess and terminal backends behind the new seams
- add native macOS and Windows watcher backends so supported hosts do not rely on polling by
  default
- add focused host-platform CI later, after the terminal/process and watcher backends are stable
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
- validate broader native file-watch coverage beyond the current Linux-backed asset watcher once
  target-host build and runtime validation are available; until then snapshot fallback remains
  the correct baseline for macOS and Windows hosts
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
  - `workspace/WorkspaceDapManager.*` manages multiple debug adapters, one per debugger type
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
  - `PluginHost` gains eight new Lua tables: `ctx.formatters` (add), `ctx.save_participants` (add),
    `ctx.completion` (add), `ctx.code_actions` (add), `ctx.tasks` (add), `ctx.tools` (add),
    `ctx.debuggers` (add), `ctx.tests` (add); corresponding C++ accessors are available to
    workspace coordinators
  - `PluginHost` now exposes runtime query or execution paths for save participants, completion
    providers, code-action providers, and test providers
  - `workspace/WorkspaceLspClient.*` now supports `textDocument/formatting`
  - the editor save path now runs save participants before formatter execution and writes the
    transformed buffer back into the viewport before disk save
  - `WorkspaceToolDownloader::Download(...)` is no longer a stub and now validates cached or
    local file installs
  - built-in commands, menus, and keybindings now surface completion, code actions, tasks, test
    discovery or execution, output channels, and first-pass debug start or stop through live shell
    state
  - completion and code actions now render through dedicated host-owned editor overlays, while
    task and test flows reuse the bottom panel and Tests sidebar instead of inventing parallel UI
  - runtime and shell wiring are covered in `tests/PluginHostTests.cpp` and
    `tests/WorkspaceShellPluginTests.cpp`, in addition to `tests/Phase3Tests.cpp`
  - Phase 5 validation now exercises plugin-declared language servers end to end for diagnostics,
    completion, code actions, go-to-definition, references, and editable merge-buffer lifecycle
    in `tests/Phase5Tests.cpp`

Open work:

- keep validating real LSP and DAP server communication beyond the shipped end-to-end fake-server
  coverage before promising broader language-server or debugger coverage
- keep the completion and code-action overlays host-owned and minimal; do not fork the command
  prompt into a second editor interaction model
- extend test UX only after real controller state exists for richer tree, gutter, and per-test
  debug workflows
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
  - `workspace/WorkspaceAuthProvider.*` manages authentication providers and active sessions
  - `workspace/WorkspaceSecretStorage.*` now persists host-managed secrets in the config
    directory; it is still not backed by an OS credential manager
  - `PluginHost` gains four new Lua tables: `ctx.scm` (add), `ctx.annotations` (add),
    `ctx.auth` (add); virtual documents and review comments are host-managed
  - `WorkspaceShell` rebuilds SCM, annotation, and auth registries from plugin contributions on
    reload, and virtual documents now open and refresh in the live tab model
  - the Git sidebar now shows SCM and auth summary lines, review comments render as gutter markers
    in editor and virtual-document views, and auth login, refresh, or logout now flow through
    built-in commands plus host-owned output channels
  - direct coverage now exists in `tests/WorkspaceShellPluginTests.cpp` and
    `tests/Phase4Tests.cpp`

Open work:

- keep the built-in Git compare and stage flows host-owned until a cohesive provider-driven source
  control design is ready
- extend review UX from gutter markers to richer thread panels, compose, edit, and resolve only
  after location mapping and persistence rules are stable
- add an OS credential backend for `WorkspaceSecretStorage` when the deployment targets and secure
  storage contract are ready
- validate GitLens-like and GitHub-review-like workflows against the current provider seams before
  broadening them

- Phase 5 AI and LLM runtime surfaces are retired from the product scope.

### 6. Deferred Work And Throughput Pass (2026-05-02)

This phase addresses background-thread isolation, event-driven file watching, search throughput,
and adaptive idle rendering. The infrastructure layer is shipped; workspace wiring is in progress.

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

Open work (tracked in `openspec/changes/deferred-work-and-throughput-pass/tasks.md`):

- wire `FileIndexWatcher` to project open/close in the workspace coordinator (task 2.2–2.3) and
  update file-finder and search call sites to consume `ProjectFileIndex::Snapshot()` (tasks 2.4–2.5)
- migrate git `Status`, `Blame`, and `Log` call sites through `ProjectBackgroundExecutor` with SDL
  user-event delivery (tasks 3.2–3.4); verify cancel-on-project-switch (task 3.5)
- add `layout_dirty_` flag and guard `ComputeLayout` in `PrepareFrameOnce` (tasks 5.1–5.2); add
  `visible_line_range` to `FrameToken` and plumb render phases through it (tasks 5.3–5.4)
- add `SearchResultBuffer` and incremental search streaming with per-batch SDL wake events (tasks
  7.1–7.4); update cancel path and add integration tests (tasks 7.5–7.6)
- add `IdleHint` enum and replace zero-delay SDL poll loop with `IdleHint`-driven strategy (tasks
  8.5–8.6); run `idle_soak_30s` harness to verify near-zero wake rate at rest (task 8.8)

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
  findings from that pass are now confirmed fixed; see `docs/performance-findings.md`
  (Deep-Dive Findings section) for the full record
- a 2026-04-23 second static pass confirmed all previous fixes and found four new bottlenecks;
  the review-comment marker, terminal snapshot generation, editor-pane-layout, and terminal cursor
  lock items from that pass are now fixed;
  see `docs/performance-findings.md` (Second Performance Pass section) and
  `docs/known-tech-debt.md` items 8–11 for the prioritized list

Open work:

- add regression tests whenever a bug is fixed; do not rely on “should be covered already”
- keep retained redraw comparison tests serial under SDL dummy video because they share global SDL state
- keep profiling startup, redraw, typing, scrolling, and idle behavior with the tracing docs in
  this directory
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

- full debugger UI beyond first-pass start or stop and output-channel plumbing
- plugin marketplaces, remote install flows, and Micro-plugin compatibility
- cloud or collaboration features
- recent-project and recent-file affordances
- diagnostics as an implicit requirement; diagnostics only if a dedicated diagnostics phase is started

## Companion Docs

Keep these when you need deeper design context:

- `openspec/specs/product-vision/spec.md`: authoritative product thesis and non-goals
- `openspec/specs/diff-merge-editor/spec.md`: durable compare and merge behavioral contract
- `openspec/specs/performance-budgets/spec.md`: durable performance budget policy
- `AGENTS.md`: repo-level engineering policy, iteration loop, and agent expectations
- `docs/implementation-guide.md`: durable product direction
- `docs/plugin-runtime-research.md`: deeper plugin architecture notes and external references
- `docs/known-tech-debt.md`: concrete open debt still worth preserving
- `docs/text-surface-unification.md`: durable text-input and navigable-text interaction contract
- `docs/performance-findings.md`: concrete shipped performance wins worth preserving
- `docs/startup-tracing.md`: startup profiling workflow
- `docs/runtime-profiling.md`: runtime and redraw profiling workflow

Archived (shipped or superseded):
- `docs/archive/plugin-platform-expansion-plan.md`: plugin platform planning — shipped across Phases 1–5
- `docs/archive/production-tech-debt-review.md`: 2026-04-20 structural debt review — major items resolved by the shell-breakdown pass
