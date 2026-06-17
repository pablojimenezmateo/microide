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

The closed tech-debt history (previously a long journal inside
`dev-docs/project/known-tech-debt.md`) was moved here on 2026-06-17:

- `archive/2026-04-29-comprehensive-cleanup-and-single-line-input.md`
- `archive/2026-05-01-render-and-layout-perf-batch.md`
- `archive/2026-05-15-layout-revision-tiers.md`
- `archive/2026-05-19-search-index-event-watch.md`
- `archive/2026-05-19-throughput-pass-followups.md`
- `archive/2026-05-20-textviewport-and-shell-decomposition.md`
- `archive/2026-06-11-deep-correctness-audit.md`
- `archive/2026-06-15-render-app-util-terminal-pass.md`
- `archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md`

Two of these carry durable **do-not-retry** guardrails (the editor glyph-atlas draw-path and the
`TextDocumentModel` ownership extraction); `dev-docs/project/known-tech-debt.md` links them directly.
