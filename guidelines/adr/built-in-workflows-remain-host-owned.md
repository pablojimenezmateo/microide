# ADR-002: Built-In IDE Workflows Remain Host-Owned

Purpose: record the durable policy for core product ownership in `microide`.

- Status: Accepted
- Date: 2026-04-24
- Owners: microide maintainers
- Tags: workspace, rendering, plugins
- Related:
  - `AGENTS.md`
  - `../architecture.md`
  - `../../dev-docs/project/active-work.md`
  - `../../dev-docs/project/implementation-guide.md`

## Context

The product is centered on built-in editor, compare, merge, search, git, and terminal workflows. Plugin expansion is important, but those core flows define the product and share host-owned rendering, state, and performance requirements.

## Decision

Core IDE workflows remain built in and host-owned. Plugins may extend those workflows through narrow seams, but they do not replace ownership of the editor model, compare or merge surfaces, search UI, git UI, terminal UI, or top-level shell composition.

## Options Considered

1. Allow plugins to replace core workflows directly
2. Keep core workflows built in and let plugins extend around them
3. Freeze plugin scope to syntax and commands only

Option 1 was rejected because it would force the host to expose broad internal contracts and would weaken correctness and performance guarantees.

Option 3 was rejected because it would be unnecessarily restrictive for the current plugin phase.

Option 2 was chosen because it keeps the product coherent while still allowing meaningful extension.

## Consequences

### Positive

- Core product behavior stays consistent and measurable.
- Plugin APIs can remain narrower than the full host.
- Performance-sensitive UI paths remain under host control.

### Negative

- Some advanced plugin ideas will need to be reframed as structured contributions instead of custom UI takeovers.

### Follow-up

- Keep adding extension seams where real plugins need them.
- Remove compatibility shims that preserve old internal boundaries once better host APIs exist.

## Validation

- Built-in workflows continue to operate without plugin presence.
- Plugin additions do not require surrendering render-path ownership.
- Refactors of core IDE surfaces remain possible without breaking a broad plugin surface.
