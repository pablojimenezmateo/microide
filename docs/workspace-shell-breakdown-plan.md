# WorkspaceShell Breakdown Plan

Reviewed on 2026-04-20.

## Status

Phase 1 is materially advanced but not complete.

The implemented work has finished the first ownership rewrite step that actually existed in code:

- workspace state models no longer live inline in `WorkspaceShell.h`
- `WorkspaceContext` owns project-catalog, active-project, prompt, menu, interaction, and
  text-input state
- `WorkspaceShell` no longer keeps the old reference-alias member block for active-project state
- top-level controllers and input or mouse coordinators no longer depend on `WorkspaceShell&`
- `WorkspaceActionCoordinator` no longer depends on `WorkspaceShell&`; it now executes through
  `WorkspaceActionContext`
- ordinary production `friend class` access on `WorkspaceShell` is gone

The remaining `MICROIDE_TESTING` friend for `WorkspaceShellTestAccess` is test-only and does not
participate in production behavior.

What is still open:

- `WorkspaceShell` is still a large facade with app-facing orchestration, rendering, and action
  enablement logic
- there is still no dedicated view tree or bootstrapper layer
- tests still lean heavily on `WorkspaceShellTestAccess`
- action routing is context-backed now, but there is still no command bus or event bus

## Final State

### State Ownership

`WorkspaceShell` still acts as the app-facing facade used by `src/app/*`, but durable workspace
state now lives under `WorkspaceContext` and `ProjectWorkspaceState`.

Current ownership split:

- `WorkspaceContext`
  - project catalog
  - active project storage
  - prompt state
  - menu state
  - transient interaction state, including tab-drag state
  - text-input and IME state
- `ProjectWorkspaceState`
  - project root
  - directory tree and file index
  - open tabs and active tab index
  - sidebar, overlay, and panel state
  - terminal tabs
  - diagnostics and project-local editor preferences

`WorkspaceShell` now reaches through `WorkspaceContext` directly instead of keeping the old
reference-alias member block for active-project state.

### Controller Boundaries

The following controllers now bind through explicit state plus callback dependencies instead of
through `WorkspaceShell&`:

- `WorkspaceProjectCatalogCoordinator`
- `WorkspacePersistenceCoordinator`
- `WorkspaceLifecycleCoordinator`
- `WorkspaceDirtyPromptCoordinator`
- `WorkspaceMenuCoordinator`
- `WorkspaceCommandPromptCoordinator`
- `WorkspaceDiffTabCoordinator`
- `WorkspaceCompareInteractionCoordinator`
- `WorkspacePathMutationCoordinator`
- `WorkspaceSidebarCoordinator`
- `WorkspaceKeyInputCoordinator`
- `WorkspaceTextInputCoordinator`
- `WorkspaceChromeMouseCoordinator`
- `WorkspaceSidebarMouseCoordinator`
- `WorkspacePanelMouseCoordinator`
- `WorkspaceActionContext`
- `WorkspaceTabCoordinator`
- `WorkspaceEditorMouseCoordinator`
- `WorkspaceCompareMouseCoordinator`
- `WorkspaceMergeMouseCoordinator`
- `WorkspaceTabMouseCoordinator`

This leaves `WorkspaceShell` as a facade and wiring site rather than the universal dependency for
every workspace subsystem, but it is still larger than the intended end state.

### Action Routing

`WorkspaceActionCoordinator` still provides top-level dispatch, but it now runs entirely through a
callback-backed `WorkspaceActionContext` value instead of taking `WorkspaceShell&` or reaching
through shell-private state.

## What Landed

The breakdown completed in coherent slices:

1. extracted workspace state models into dedicated `Workspace*State.h` files
2. introduced `WorkspaceContext` and moved active-workspace ownership into it
3. migrated project-catalog, persistence, lifecycle, dirty-prompt, menu, command-prompt,
   compare, path-mutation, sidebar, key-input, text-input, and initial mouse coordinators off
   `WorkspaceShell&`
4. finished the remaining tab and mouse coordinators
5. moved tab-drag interaction state into `WorkspaceInteractionState`
6. removed the remaining production `friend class` declarations from `WorkspaceShell`
7. removed the old active-project reference aliases from `WorkspaceShell`
8. moved `WorkspaceActionCoordinator` off `WorkspaceShell&`

## Remaining Work

The current code still falls short of the end-state architecture in a few important ways:

- `WorkspaceShell` is still the dominant render and event orchestrator
- `IsActionEnabled` still lives on the shell
- no `WorkspaceRootView` or equivalent view-tree layer exists yet
- no dedicated bootstrapper or composition-root type exists yet
- `WorkspaceShellTestAccess` is still broad and heavily used

## Success Criteria

The work completed in this phase because all of the following are now true:

- `WorkspaceShell` no longer requires ordinary production `friend class` access
- workspace state models live outside `WorkspaceShell.h`
- `WorkspaceShell` no longer keeps the old active-project reference alias block
- controller and coordinator construction uses explicit state plus callback seams instead of full
  shell reach-through
- action, tab, compare, merge, and mouse routing no longer depend on `WorkspaceShell&`
- the codebase builds and the full test suite passes with the new seams

## Follow-On Work

Future workspace refactors should start from the current seams instead of reopening shell-wide
access. In particular:

- keep new controllers state-scoped and callback-scoped
- avoid adding new `friend` access for production code
- prefer moving behavior into focused services or controllers over widening `WorkspaceShell`
- treat `WorkspaceShellTestAccess` growth as technical debt, not as a default extension point
- if the remaining render or orchestration work is split further, prefer a host-owned view tree or
  composition root over rebuilding another shell-sized facade elsewhere
