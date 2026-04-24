# Tech Debt Index

Purpose: store durable records of intentional shortcuts, deferred cleanup, missing abstractions, and known gaps that should stay visible after a change ships.

## Naming

- Format: `<short-title>.md`
- Example: `workspace-shell-boundary.md`
- Template: `template.md`

## When To Create A Record

Create a record when a change introduces or uncovers debt such as:

- an intentional shortcut taken to ship safely in stages
- a known gap between the current implementation and the desired architecture
- a temporary workaround for tooling, platform, plugin, or persistence issues
- a missing abstraction or test seam that should be fixed later
- a performance caveat that is accepted for now but should remain visible

Use this registry for debt that should remain visible after the current change is complete.

## Open Items

No open tech debt records yet.

## Archive

Resolved records belong in `archive/` using the file name `YYYY-MM-DD-<slug>.md`.
