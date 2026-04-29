# Architecture Guide

Purpose: define the durable system shape, subsystem boundaries, and integration philosophy for `microide`.

## Quick Scan

- `microide` is a native desktop IDE shell, not a service platform.
- The host is intentionally small and explicit: built-in workflows stay built in, and plugins extend narrow registries and services.
- The authoritative product scope (in-scope capabilities, non-goals, priority order) lives in `openspec/specs/product-vision/spec.md`.
- The durable diff and merge behavioral contract lives in `openspec/specs/diff-merge-editor/spec.md`.
- Presentation and orchestration live primarily in `src/workspace`.
- Core editor, compare, and terminal behavior stays in focused model or helper modules.
- OS, filesystem, subprocess, git, search, and plugin-runtime integrations stay behind narrow service boundaries.
- Prefer correcting bad ownership boundaries over preserving temporary or accidental compatibility.

## System Shape

```text
src/app
  -> application bootstrap, SDL lifetime, event loop

src/workspace
  -> shell composition, state, actions, menus, overlays, routing, redraw coordination

src/editor
  -> text viewport, layout, syntax, diagnostics rendering support

src/compare
  -> diff and merge models

src/terminal
  -> PTY-backed terminal session model

src/project
  -> file operations, indexing, search, git, project-root services

src/plugin
  -> plugin host and plugin-facing contribution boundaries

src/platform
  -> filesystem, subprocess, file watching, app-directory integration

src/render
  -> text backends, themes, render primitives

src/util
  -> focused shared helpers with no hidden product ownership
```

## Architectural Layers

The codebase is not organized as a framework-heavy layered stack, but it does have durable ownership seams:

- Presentation and orchestration:
  - `src/workspace/*`
  - `WorkspaceRootView`, render composition helpers, event and action coordinators
- Core models and deterministic logic:
  - `src/editor/*`
  - `src/compare/*`
  - `src/terminal/*`
  - pure or mostly pure helpers in `src/util/*`
- Integration and infrastructure:
  - `src/project/*`
  - `src/platform/*`
  - `src/plugin/*`
  - SDL-facing bootstrap in `src/app/*`

Keep these boundaries obvious. UI code should not grow direct knowledge of git command construction, filesystem mutation details, PTY plumbing, or plugin runtime internals.

## Boundary Rules

- `WorkspaceShell` is an app-facing facade, not the extension surface for every new feature.
- If work needs new shell behavior, prefer a focused coordinator, registry, state model, or service instead of adding another wide `WorkspaceShell` method cluster.
- Render paths stay host-owned. Plugins contribute data and actions; the host decides layout, paint order, and visual semantics.
- External tool boundaries belong in `src/project/*` or `src/platform/*`, where the UI can consume stable results without caring how they were produced.
- Shared helpers in `src/util/*` should stay small and reusable. Do not move subsystem ownership into generic buckets just to avoid creating a focused file.

## Enforced Invariants

These are checked by the `ArchitectureInvariants/SoftChecks` test in `microide_tests` (registered via `tests/ArchitectureInvariantsTests.cpp` and run as part of the default `ctest` entrypoint). The rules below are hard-fail unless the test source explicitly marks them otherwise.

- New numeric parsing in `src/` must use `src/util/Parse.{h,cpp}`. Do not add `try`/`catch` wrappers around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, `std::stod`, or similar APIs.
- Workspace source under `src/workspace/*` must not add `friend class` or `friend struct` declarations.
- Coordinator headers matching `src/workspace/Workspace*Coordinator*.h` must not take `WorkspaceShell&` or `WorkspaceShell*` in constructors. Inject the narrow service or callback dependency instead.
- `src/workspace/WorkspaceShell.h` stays at or below 400 lines and `src/workspace/WorkspaceShell.cpp` at or below 600 lines.
- The render translation units listed in `CheckRenderSurfaceStateAccess` (`WorkspaceShellRenderFrame`, `WorkspaceShellRenderOverlay`, `WorkspaceShellRenderTextInput`, `WorkspaceShellRenderSidebar`, `WorkspaceShellRenderBottomPanel`, `WorkspaceShellHoverPopup`, `WorkspaceShellHoverTargets`) must not read `context_.current_project_state` or call `CurrentTextInputSurface(...)`. They consume view models built by `RenderViewModelBuilder`.
- No single `src/plugin/*.cpp` translation unit may exceed 800 lines (hard-fail).
- Workspace coordinator translation units matching `src/workspace/Workspace*Coordinator*.cpp` are capped at 800 lines; split by ownership seam instead of extending monoliths.

When extending these rules, modify `tests/ArchitectureInvariantsTests.cpp` in the same change so the invariant is enforced by CI rather than by review alone.

## Workspace Service Model

The workspace decomposes into a small set of services owned by the shell and consumed by coordinators through interface references. The canonical service set is:

- `EditorTabService` — tab list, active tab, splits, dirty state, save/load, view restore. Owns the active editor viewport (no shell-level fallback).
- `ProjectCatalogService` — open projects, switch, close, project-state activation.
- `PromptSurfaceService` — prompt, dirty-prompt, and path-mutation prompt lifecycle.
- `SidebarService` — sidebar mode, refresh, open-or-select.
- `CompareMergeService` — compare/merge tab orchestration and navigation.
- `TerminalPanelService` — terminal tabs, focus, panel layout requests.
- `PluginRuntimeService` — plugin lifecycle, asset reload, output channels.
- `PersistenceService` — sole owner of disk I/O for project state, user config, workspace session, and conversation registry. Routes through `PersistedRecordReader`/`PersistedRecordWriter`.
- `RenderViewModelBuilder` — produces typed POD-like view-model structs per render surface from service queries.

Rules:

- Coordinators take only the service interfaces they need, value-typed input state, and read-only resource handles. They never take `WorkspaceShell&`, never hold a shell pointer, and never reach into shell-private state.
- Services own their state and the only mutation API for it. If a coordinator needs a behavior the service does not expose, add the method to the service contract; do not add a friend escape hatch.
- Render code consumes view-model parameters only. View models are POD-like, trivially copyable, and contain exactly the fields the surface needs. They do not hold pointers or references to the shell, coordinators, or services.
- Single-line shell inputs use `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler`. Do not duplicate insert / backspace / delete / movement / selection / clipboard / select-all logic per surface. The chat composer is the only documented multiline exception.
- `lua_State*` lifecycle lives in `plugin/LuaRuntime` only. Plugin extension-surface modules consume the runtime through opaque handles.
- Persistence for project state, user config, session restore, and chat conversations always routes through `PersistenceService`. Do not hand-roll a new text format or open these files directly from elsewhere.

## Plugin-Phase Direction

Plugin expansion is a major current phase. The durable direction is:

- host-owned registries for commands, sidebars, settings, status items, tools, tasks, and related contributions
- plugin lifecycle and bookkeeping handled by a dedicated runtime path
- asset loading and reload behavior handled by the host
- built-in editor, compare, merge, search, git, terminal, and diagnostics flows remain built in

Do not expose raw coordinator or shell internals just because a plugin can use them today. If a real plugin needs a capability, add a narrow host API that models the capability directly.

## Ownership And Dependencies

- Prefer explicit dependencies through parameters, constructor state, or small callback bundles.
- Keep deterministic state transitions close to the state they affect.
- Avoid hidden coupling through mutable globals, cross-cutting helper singletons, or wide friend access.
- If a file grows because two subsystems are tightly interleaved, separate the API boundary first and then move logic across it.

## When To Capture A Decision

Create an ADR when work changes a durable policy such as:

- plugin extension boundaries
- host versus plugin ownership
- render-path architecture
- persistence or workspace-state shape
- external tool strategy
- a major performance policy or measurement requirement

Use `adr/template.md` as the starting point.
