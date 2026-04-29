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

- `microide_tests` includes an `ArchitectureInvariants` fixture and it runs through the default `ctest` entrypoint.
- New numeric parsing in `src/` must use `src/util/Parse.{h,cpp}`. Do not add `try`/`catch` wrappers around `std::stoi`, `std::stoll`, `std::stoull`, `std::stof`, `std::stod`, or similar APIs.
- Workspace source under `src/workspace/*` should not add `friend class` or `friend struct` declarations.
- Coordinator headers under `src/workspace/Workspace*Coordinator*.h` should not take `WorkspaceShell&` or `WorkspaceShell*` in constructors; inject the narrow service or callback dependency instead.
- No single `src/plugin/*.cpp` translation unit should exceed 800 lines.
- `src/workspace/WorkspaceShell.h` should stay at or below 400 lines and `src/workspace/WorkspaceShell.cpp` at or below 600 lines.
- Some invariants still warn instead of hard-failing while the cleanup change is in flight. When a slice removes the last known violation for a rule, flip that rule to hard-fail in the same change.

## Workspace Service Model

- Workspace behavior should be service-oriented: services own state and mutations, coordinators translate input into service intents.
- Render code should consume typed view models from `RenderViewModelBuilder`; render surfaces should not reach into mutable shell state.
- Single-line shell inputs should use shared `editor/SingleLineEditor` plus `editor/SingleLineKeyHandler` for standard edit behavior.
- Persistence should use the shared typed record format through `PersistedRecordReader` and `PersistedRecordWriter`, with `PersistenceService` as the workspace entrypoint.

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
