# WorkspaceShell Breakdown Plan

Reviewed on 2026-04-20.

## Status

The planned `WorkspaceShell` breakdown work is implemented.

The implemented work now covers the full shell-shaping scope that exists in the codebase:

- workspace state models no longer live inline in `WorkspaceShell.h`
- `WorkspaceContext` owns project-catalog, active-project, prompt, menu, interaction, and
  text-input state
- `WorkspaceShell` no longer keeps the old reference-alias member block for active-project state
- top-level controllers and input or mouse coordinators no longer depend on `WorkspaceShell&`
- `WorkspaceActionCoordinator` no longer depends on `WorkspaceShell&`; it now executes through
  `WorkspaceActionContext`
- top-level action enablement now runs through `WorkspaceActionAvailability`
- top-level SDL event routing now runs through `WorkspaceEventDispatcher`
- scheduled wake handling now runs through `WorkspaceWakeController`
- top-level render composition now runs through `WorkspaceRootView`
- `WorkspaceRootView` now composes dedicated chrome, sidebar, overlay, panel, menu, prompt, and
  active-surface view classes
- a dedicated `WorkspaceShell::Bootstrapper` now acts as the shell composition root for action,
  render, and event wiring
- ordinary production `friend class` access on `WorkspaceShell` is gone
- test-only friend access is gone too; tests now use the `MICROIDE_TESTING`-gated public
  `WorkspaceShell::TestAccess` API from `workspace/WorkspaceShellTesting.h`

The shell is still a large app-facing facade, but its remaining size now reflects broad product
surface area rather than direct coordinator, render-tree, or friend-based ownership of everything.

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

Action enablement is no longer shell-owned either. `WorkspaceActionAvailability` now evaluates the
top-level `ActionId` enablement rules from `WorkspaceContext` plus focused read-only callbacks for
tree, editor, compare, and terminal state instead of routing that logic through
`WorkspaceShell::IsActionEnabled`.

The action and event architecture intentionally uses explicit coordinators and dispatchers instead
of introducing a generic command bus or event bus. `WorkspaceActionCoordinator`,
`WorkspaceActionContext`, `WorkspaceActionAvailability`, `WorkspaceEventDispatcher`, and
`WorkspaceWakeController` are the chosen explicit seams.

### Render Ownership

`WorkspaceShell` still exposes the app-facing `Render` and `RenderPrepared` entry points, but the
top-level render composition path now runs through `WorkspaceRootView`. That view owns the ordered
render phases for frame preparation, active workspace surfaces, chrome, sidebar, overlay, bottom
panel, menus, prompts, and text-input composition.

`WorkspaceRootView` now composes dedicated `WorkspaceActiveSurfaceView`, `WorkspaceChromeView`,
`WorkspaceSidebarView`, `WorkspaceOverlayView`, `WorkspacePanelView`, `WorkspaceMenuView`, and
`WorkspacePromptView` types. `WorkspaceShell::Bootstrapper` owns the composition of that view tree
alongside the shell's action and event wiring.

### Test Access

The old test-only `WorkspaceShellTestAccess` friend is gone. Shell tests now include
`workspace/WorkspaceShellTesting.h` and use the `MICROIDE_TESTING`-gated public
`WorkspaceShell::TestAccess` API instead of reaching through a private friend helper declared in
`tests/`.

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
9. moved top-level action enablement into `WorkspaceActionAvailability`
10. introduced a minimal `WorkspaceRootView` and routed `Render` or `RenderPrepared` through it
11. introduced `WorkspaceEventDispatcher` and `WorkspaceWakeController` for shell event
    orchestration
12. expanded `WorkspaceRootView` into a composed view tree and added `WorkspaceShell::Bootstrapper`
13. replaced the friend-based `WorkspaceShellTestAccess` helper with the public
    `WorkspaceShell::TestAccess` testing API and deleted the old test header

## Success Criteria

The work completed in this phase because all of the following are now true:

- `WorkspaceShell` no longer requires ordinary production `friend class` access
- `WorkspaceShell` no longer uses test-only friend access either
- workspace state models live outside `WorkspaceShell.h`
- `WorkspaceShell` no longer keeps the old active-project reference alias block
- controller and coordinator construction uses explicit state plus callback seams instead of full
  shell reach-through
- action, tab, compare, merge, and mouse routing no longer depend on `WorkspaceShell&`
- top-level action enablement no longer lives on `WorkspaceShell`
- top-level event and wake routing no longer live directly inside `WorkspaceShell`
- top-level render composition now runs through a composed host-owned view tree
- a dedicated shell bootstrapper or composition root exists for action, render, and event wiring
- the codebase builds and the full test suite passes with the new seams

## Follow-On Work

Future workspace refactors should start from the current seams instead of reopening shell-wide
access. In particular:

- keep new controllers state-scoped and callback-scoped
- avoid adding new `friend` access for production code
- prefer moving behavior into focused services or controllers over widening `WorkspaceShell`
- keep the `WorkspaceShell::TestAccess` surface narrow and only extend it when a test cannot be
  expressed through public behavior or a smaller subsystem seam
- if the render tree grows further, preserve the host-owned composed-view model instead of
  rebuilding another shell-sized facade elsewhere
