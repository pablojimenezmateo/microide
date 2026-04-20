# WorkspaceShell Breakdown Plan

Reviewed on 2026-04-20.

## Status

This cleanup is complete for the current codebase.

The original plan started as a broad ownership rewrite for `WorkspaceShell`, including possible
view-tree and bootstrapper extractions. The implemented work finished the production problem that
actually existed:

- workspace state models no longer live inline in `WorkspaceShell.h`
- `WorkspaceContext` owns project-catalog, active-project, prompt, menu, interaction, and
  text-input state
- top-level controllers and input or mouse coordinators no longer depend on `WorkspaceShell&`
- ordinary production `friend class` access on `WorkspaceShell` is gone

The remaining `MICROIDE_TESTING` friend for `WorkspaceShellTestAccess` is test-only and does not
participate in production behavior.

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

`WorkspaceShell` still keeps reference aliases onto the active project state for facade
implementation convenience, but those aliases are no longer used as friend-only coordinator
backdoors.

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
every workspace subsystem.

### Action Routing

`WorkspaceActionCoordinator` still provides top-level dispatch, but its domain surface now runs
through a callback-backed `WorkspaceActionContext` instead of reaching through shell-private state.
That preserves the existing action architecture while removing the last production friend-only
action seam.

## What Landed

The breakdown completed in coherent slices:

1. extracted workspace state models into dedicated `Workspace*State.h` files
2. introduced `WorkspaceContext` and moved active-workspace ownership into it
3. migrated project-catalog, persistence, lifecycle, dirty-prompt, menu, command-prompt,
   compare, path-mutation, sidebar, key-input, text-input, and initial mouse coordinators off
   `WorkspaceShell&`
4. finished the remaining action, tab, and mouse coordinators
5. moved tab-drag interaction state into `WorkspaceInteractionState`
6. removed the remaining production `friend class` declarations from `WorkspaceShell`

## Explicitly Closed Items

The original draft proposed additional work such as:

- a root view tree
- a dedicated bootstrapper or composition root
- large-scale migration away from `WorkspaceShellTestAccess`

Those ideas were evaluated during the cleanup and are not required to complete the current
ownership rewrite. They remain optional future refactors, not active plan items. Keeping them in
this document as required work would leave split-brain guidance, so this plan now records the
landed architecture instead of preserving speculative future phases.

## Success Criteria

The cleanup is complete because all of the following are now true:

- `WorkspaceShell` no longer requires ordinary production `friend class` access
- workspace state models live outside `WorkspaceShell.h`
- controller and coordinator construction uses explicit state plus callback seams instead of full
  shell reach-through
- action, tab, compare, merge, and mouse routing no longer depend on shell-private coordinator
  access
- the codebase builds and the full test suite passes with the new seams

## Follow-On Work

Future workspace refactors should start from the current seams instead of reopening shell-wide
access. In particular:

- keep new controllers state-scoped and callback-scoped
- avoid adding new `friend` access for production code
- prefer moving behavior into focused services or controllers over widening `WorkspaceShell`
- treat `WorkspaceShellTestAccess` growth as technical debt, not as a default extension point
