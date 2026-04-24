# ADR-001: Plugin Contributions Flow Through Host-Owned Registries

Purpose: record the durable policy for how plugins extend `microide`.

- Status: Accepted
- Date: 2026-04-24
- Owners: microide maintainers
- Tags: plugins, workspace
- Related:
  - `AGENTS.md`
  - `../architecture.md`
  - `../../docs/active-work.md`
  - `../../docs/plugin-runtime-research.md`

## Context

`microide` is in an active plugin-expansion phase, but the host still prioritizes correctness, performance, and narrow ownership. Exposing `WorkspaceShell` or raw coordinator internals to plugins would freeze the wrong boundaries and make future refactors harder.

## Decision

Plugins extend the host through explicit, host-owned registries and services. New plugin capabilities should be modeled as narrow contribution types such as commands, sidebar providers, diagnostics, hovers, tasks, or tools.

The host owns:

- plugin discovery and lifecycle
- validation of plugin contributions
- rendering and presentation of plugin-contributed data
- persistence and workspace-state integration

## Options Considered

1. Expose `WorkspaceShell` directly to plugins
2. Use host-owned registries and narrow service APIs
3. Keep plugins limited to one generic script hook

Option 1 was rejected because it would lock in accidental shell structure and create broad hidden coupling.

Option 3 was rejected because it would keep the host small, but it would not model real extension needs clearly enough.

Option 2 was chosen because it supports real plugin features while preserving refactorable host boundaries.

## Consequences

### Positive

- The host can keep refactoring internal coordinators and state models.
- Plugin APIs stay explicit and reviewable.
- Rendering remains consistent and host-controlled.

### Negative

- New plugin capabilities require deliberate host API design work.
- Some plugin feature requests will require boundary changes before implementation.

### Follow-up

- Dogfood new seams with repo-owned plugins before widening them further.
- Keep plugin runtime docs aligned with the current contribution surface.

## Validation

- Plugins can add useful capabilities without new raw shell access.
- Internal refactors do not require broad plugin API churn.
- Host-owned rendering and workspace-state invariants remain intact.
