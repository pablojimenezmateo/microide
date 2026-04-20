# WorkspaceShell Breakdown Plan

Reviewed on 2026-04-20.

## Scope

This plan covers the remaining architectural work needed to stop `WorkspaceShell` from acting as
the central owner, router, state bag, and friend-only backdoor for most of `src/workspace/*`.

Primary code areas:

- `src/workspace/WorkspaceShell.h`
- `src/workspace/WorkspaceActionContext.*`
- `src/workspace/WorkspaceActionCoordinator.*`
- `src/workspace/Workspace*Coordinator*`
- `src/workspace/WorkspaceShellRender*`
- `src/workspace/WorkspaceShellMouse*`
- `src/workspace/WorkspaceLifecycleCoordinator.*`
- `tests/WorkspaceShellTestAccess.h`

This document is intentionally broader than a local cleanup. The goal is to finish the structural
rewrite, not to preserve the current shell shape with smaller files.

## Current State

Recent work already removed the old helper bucket and moved real logic into narrower modules such
as `WorkspacePluginRuntime*`, `WorkspaceProjectSearchRuntime*`, `WorkspaceLayout*`,
`WorkspaceTerminalSelection*`, `WorkspaceActionTypes*`, `WorkspaceMenuRegistry*`, and the various
top-level coordinators.

That cleanup was necessary, but it did not finish the architecture change. The central problem is
still ownership.

Evidence from the current codebase:

- `src/workspace/WorkspaceShell.h` is still about 1.2k lines even after moving project, tab,
  prompt, menu, and interaction state model definitions into dedicated `Workspace*State.h`
  headers, and it still defines a large amount of workspace state, UI state, render state, and
  interaction state.
- `WorkspaceShell` still embeds `ProjectWorkspaceState current_project_state_` and then re-exposes
  it through alias members such as `project_root_`, `directory_tree_`, `open_tabs_`, `surface_`,
  `sidebar_state_`, `overlay_state_`, `panel_state_`, `terminal_tabs_`, `diagnostics_store_`,
  `active_colorscheme_name_`, and `editor_preferences_`.
- `WorkspaceShell` still declares 15 `friend class` relationships so coordinators can reach into
  private shell state directly.
- `WorkspaceContext` now owns project catalog, active project, prompt, menu, and interaction
  state, but most top-level coordinators still take `WorkspaceShell&` and therefore still depend
  on the full shell rather than on narrow APIs. `WorkspaceProjectCatalogCoordinator`,
  `WorkspacePersistenceCoordinator`, `WorkspaceLifecycleCoordinator`, and
  `WorkspaceDirtyPromptCoordinator` plus `WorkspaceMenuCoordinator` plus
  `WorkspaceCommandPromptCoordinator` are the first exceptions: they now depend on
  `WorkspaceContext` plus explicit callbacks instead of `WorkspaceShell&`.
- `WorkspaceActionContext` is still a shell proxy. It exposes a cleaner file boundary than the old
  nested action coordinator, but it still mostly forwards into private shell state and shell
  helpers.
- `WorkspaceShell::HandleEvent`, `PrepareRenderFrame`, and `RenderPrepared` still coordinate most
  of the product through shell methods, even though their logic is split across multiple files.
- `tests/WorkspaceShellTestAccess.h` is still more than 1k lines and test code references
  `WorkspaceShellTestAccess` more than a thousand times. That is a strong signal that tests still
  need shell internals to reach real behavior.

The current shape is no longer a single translation-unit blob, but it is still a god object.

The first suggested landing-order slice is now complete: workspace state models no longer live
inline inside `WorkspaceShell.h`, and the shell now delegates the core workspace-state ownership
to `WorkspaceContext`. The remaining work is the actual ownership rewrite of more controllers,
friend removal, view extraction, and test migration.

## Decision

The project should stop trying to make `WorkspaceShell` slightly smaller and instead finish the
ownership rewrite.

The target architecture is:

- `WorkspaceShell` stays only as the application-facing facade used by `src/app/*`.
- `WorkspaceContext` owns workspace state and project catalog state.
- `WorkspaceServices` owns runtime integrations and host services.
- domain controllers mutate context through explicit APIs instead of through `friend` access.
- a root view tree owns top-level render and event routing.
- a bootstrapper wires the graph together and becomes the only place where the full dependency
  picture is assembled.

This plan does not preserve internal compatibility by default. Compatibility is only relevant at
the narrow shell facade boundary that the application loop uses.

## Design Rules

These rules apply throughout the rewrite:

- Do not create a replacement god object named `WorkspaceCore`, `WorkspaceRuntime`, or
  `WorkspaceManager`.
- Do not replace reference aliasing with a shell full of trivial getters.
- Do not introduce a global service locator. A `WorkspaceServices` aggregate may exist at wiring
  time, but individual controllers and views should receive only the dependencies they use.
- Do not introduce a stringly-typed universal event bus. Use explicit typed domain events or
  observer lists tied to real subsystem boundaries.
- Keep rendering host-owned. Plugins continue to contribute commands, providers, diagnostics,
  hover results, sidebars, and structured data, not raw SDL behavior.
- Keep async work behind dedicated services such as search, blame, plugin runtime, or later task
  executors. Do not push thread coordination back into views or the shell facade.
- Move tests as the architecture moves. Do not leave test migration as one giant cleanup phase at
  the end.

## Target End State

### `WorkspaceShell`

`WorkspaceShell` should end as a thin facade with only the app-facing API:

- initialize and shutdown
- handle SDL events and scheduled wakes
- prepare and render frames
- expose redraw, animation-delay, and quit requests

It should not directly own project trees, tabs, overlay workflows, prompt state, plugin callbacks,
or domain-specific action logic.

### `WorkspaceContext`

`WorkspaceContext` should own durable workspace state:

- active project state
- project catalog state
- tab models
- sidebar, overlay, panel, prompt, and menu state
- transient interaction state only where it is truly workspace-owned

`WorkspaceContext` should not own SDL objects, background services, or OS integrations.

### `WorkspaceServices`

This layer should own integrations that already exist or are clearly moving in that direction:

- `WorkspacePluginRuntime`
- `WorkspaceProjectSearchRuntime`
- `project::GitBlameService`
- persistence and config I/O
- clipboard and primary-selection adapters
- window and dialog adapters
- later task-executor or file-watch services where needed

The important rule is explicit ownership, not a dynamic lookup container.

### Domain Controllers

The current coordinators should be rewritten into controllers that depend on `WorkspaceContext`
plus narrow services. Expected domains:

- project catalog and project activation
- tabs and editor splits
- sidebar workflows
- overlays and prompts
- terminal panel
- compare and merge interaction
- lifecycle and shutdown
- actions and command execution

These controllers should no longer depend on `WorkspaceShell&`.

### View Tree

The view tree should own layout-aware render and hit-testing for top-level surfaces:

- `WorkspaceRootView`
- `WorkspaceChromeView`
- `WorkspaceSidebarView`
- `WorkspaceEditorStackView`
- `WorkspacePanelView`
- `WorkspaceOverlayView`
- `WorkspacePromptView`

Top-level views may still call shared render helpers, but they should consume explicit view models
and controller APIs instead of reaching through shell-private helpers.

## Phase Plan

### Phase 1. Extract State Ownership Out Of `WorkspaceShell`

Goal:

- move real workspace state out of the shell and into `WorkspaceContext`

Work:

- introduce `WorkspaceContext` and move `ProjectCatalogState` plus active
  `ProjectWorkspaceState` ownership into it
- move nested shell state types into dedicated model headers instead of keeping them buried in
  `WorkspaceShell.h`
- split state by responsibility, for example:
  - project and tab state
  - sidebar, overlay, and panel state
  - prompt and menu state
  - transient interaction state
- remove the alias-member pattern from `WorkspaceShell`
- replace direct field aliases with explicit context accessors or references scoped to a function
- keep `ProjectWorkspaceState` as the project-owned container shape, but stop treating the shell as
  the permanent owner of the active instance

Expected file pressure:

- `src/workspace/WorkspaceShell.h`
- new context or state headers under `src/workspace/`
- `WorkspaceProjectCatalogCoordinator*`
- `WorkspacePersistenceCoordinator*`

Acceptance criteria:

- `WorkspaceShell.h` no longer owns project state through alias members
- project state models live outside `WorkspaceShell.h`
- the active workspace can be switched or persisted without shell-specific rebinding logic

### Phase 2. Replace Friend Access With Explicit Controller Dependencies

Goal:

- stop routing domain behavior through `WorkspaceShell&`

Work:

- convert current coordinators to depend on `WorkspaceContext` plus only the services they need
- remove `friend class` access from `WorkspaceShell`
- replace shell-private state mutation with explicit controller or context methods
- turn shell-owned platform hooks such as clipboard, selection, dialog, and external-open callbacks
  into narrow adapters owned outside the shell facade
- reshape `WorkspaceActionContext` so it stops being a shell wrapper and instead becomes a small
  set of explicit action dependencies

Good first slices:

- `WorkspaceProjectCatalogCoordinator`
- `WorkspacePersistenceCoordinator`
- `WorkspaceSidebarCoordinator`
- `WorkspaceTabCoordinator`
- `WorkspaceLifecycleCoordinator`

Acceptance criteria:

- new or migrated controllers no longer take `WorkspaceShell&`
- `WorkspaceShell` friend count trends to zero and finishes at zero
- shell-private field access is replaced by explicit APIs

### Phase 3. Replace Shell-Centric Action And Event Routing

Goal:

- move command enablement and command execution out of `WorkspaceShell`
- remove manual shell fan-out where one change forces shell code to notify several unrelated
  subsystems

Work:

- replace `WorkspaceShell::IsActionEnabled` with per-action handlers or handler-owned enablement
  logic
- keep `WorkspaceActionTypes*` and registries as the source of action identity, but make execution
  domain-owned
- replace `ActionCoordinator` plus `WorkspaceActionContext` with a dispatcher over domain handlers
  that depend on explicit services
- introduce typed domain events where they remove real shell coordination, for example:
  - buffer saved
  - project switched
  - path renamed or deleted
  - diagnostics changed
  - plugin runtime reloaded
  - terminal tab exited

Important constraint:

- use typed event sources or observer lists owned by the relevant service or controller
- do not add a universal catch-all event bus that recreates hidden coupling

Acceptance criteria:

- no action enablement switch remains on `WorkspaceShell`
- action execution no longer depends on shell-private state
- save, rename, diagnostics, and plugin-reload flows no longer require shell-only manual fan-out

### Phase 4. Build A Real View Tree

Goal:

- stop making `WorkspaceShell` the object that knows how every visible surface renders and receives
  input

Work:

- introduce a root view that owns top-level hit-testing and child layout assignment
- extract top-level surface views for chrome, sidebar, editor stack, panel, overlays, and prompts
- move render entry points to consume view models rather than shell-private data
- move mouse routing and keyboard focus decisions to root-view plus controller collaboration
- keep `WorkspaceLayout*` and existing render helpers where useful, but make them inputs to view
  rendering rather than methods hanging off the shell

Likely migration path:

1. extract view models first
2. move render methods next
3. move input hit-testing last

Acceptance criteria:

- `WorkspaceShell::RenderPrepared` becomes a thin wrapper around root-view rendering
- top-level mouse routing only decides which child view handles the event
- surface-specific render files no longer require broad shell-private access

### Phase 5. Bootstrap Wiring And Test Migration

Goal:

- make the new architecture durable by moving construction and tests onto the new seams

Work:

- add `WorkspaceBootstrapper` or equivalent composition root
- construct context, services, controllers, and views in one place
- keep the shell facade as the app-facing wrapper around that assembled graph
- replace `WorkspaceShellTestAccess` with smaller fixtures and test helpers for:
  - context and persistence
  - actions and command dispatch
  - sidebar workflows
  - editor stack and split logic
  - prompts and overlays
  - top-level view rendering or hit-testing
- keep a smaller set of full-shell integration tests for end-to-end regressions

Acceptance criteria:

- `tests/WorkspaceShellTestAccess.h` is deleted or reduced to a minimal facade-only helper
- most tests target controllers, services, models, or views directly
- shell integration tests cover only cross-subsystem behavior that truly needs the full stack

## Suggested Landing Order

This work should land in coherent slices rather than one giant branch:

1. Move nested state types out of `WorkspaceShell.h` without changing behavior.
   Status: complete on 2026-04-20 via `Workspace*State.h` headers for project, tab, prompt, menu,
   and interaction state.
2. Add `WorkspaceContext` and make the shell delegate state ownership to it.
   Status: in progress on 2026-04-20. `WorkspaceContext` now owns the shell's core workspace
   state, and `WorkspaceProjectCatalogCoordinator` plus `WorkspaceLifecycleCoordinator` now
   consume that context plus explicit callbacks instead of `WorkspaceShell&`.
3. Migrate the least UI-coupled controllers first: project catalog, persistence, lifecycle.
   Status: in progress on 2026-04-20. `WorkspaceProjectCatalogCoordinator`,
   `WorkspacePersistenceCoordinator`, `WorkspaceLifecycleCoordinator`, and
   `WorkspaceDirtyPromptCoordinator` plus `WorkspaceMenuCoordinator` plus
   `WorkspaceCommandPromptCoordinator` now consume `WorkspaceContext` plus explicit callbacks
   instead of `WorkspaceShell&`.
4. Migrate action handling so new controllers stop needing shell reach-through.
5. Migrate sidebar, tabs, prompts, and overlays onto explicit controller APIs.
6. Extract the root view and top-level surface views.
7. Delete the remaining shell-only compatibility methods and `friend` access.
8. Finish test migration and remove oversized shell test access helpers.

Each slice should include:

- implementation
- focused regression coverage
- doc updates where the intended architecture changed

## Risks And Failure Modes

### Risk: creating a new god object under a different name

Failure mode:

- `WorkspaceContext` or `WorkspaceServices` becomes just as broad and opaque as the old shell

Mitigation:

- keep context state-only
- keep services integration-only
- pass narrow dependencies to controllers and views

### Risk: replacing direct coupling with hidden coupling

Failure mode:

- a universal event bus or service locator hides dependencies instead of removing them

Mitigation:

- use typed event channels
- keep dependencies explicit in constructors

### Risk: leaving tests behind on shell internals

Failure mode:

- production structure improves, but tests still require huge shell-only backdoors

Mitigation:

- move tests slice by slice with each controller or view extraction
- do not accept new `WorkspaceShellTestAccess` growth

### Risk: regressions in redraw, startup, or input latency

Failure mode:

- ownership improves but frame preparation, resize, or typing paths become slower

Mitigation:

- measure before and after sensitive slices with `docs/startup-tracing.md` and
  `docs/runtime-profiling.md`
- keep render and input hot paths explicit while moving ownership

## Success Criteria

The plan is complete when all of these are true:

- `WorkspaceShell` is only a thin app-facing facade
- workspace state lives in `WorkspaceContext`, not in shell aliases
- controllers and actions no longer take `WorkspaceShell&`
- no `friend` relationship is required for ordinary workspace behavior
- top-level rendering and event routing flow through a root view tree
- plugin-facing behavior still goes through host-owned registries and services rather than shell
  internals
- tests mostly target focused controllers, views, models, and services instead of shell-private
  access

## Non-Goals

This plan does not require:

- exposing `WorkspaceShell` or coordinator internals to plugins
- replacing built-in editor, compare, merge, search, git, or terminal features with plugin-owned
  surfaces
- adding async plugin execution everywhere by default
- preserving internal API compatibility during the rewrite
