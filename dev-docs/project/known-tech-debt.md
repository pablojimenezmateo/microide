# MicroIDE Known Tech Debt

Reviewed on 2026-06-17.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
It is deliberately short. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

None as of 2026-06-17. Every previously tracked topic is closed — either completed or explicitly
marked won't-do. When new debt is uncovered, add it here as a concise, actionable entry (impact,
relevant code, proposed shape) and remove it once it ships, archiving the detail.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry` per-quad). Rejected: it
  regressed every software-renderer paint scenario (+48% to +83% wall) because the composite texture
  cache already runs at >99% hit rate and `DrawString` works at the run level, not the cell level. Do
  not revisit unless **all three** preconditions hold: a GPU backend (not software), a measured
  fixture where `render.text_texture_cache_misses / cells_visited` exceeds ~10% steady-state, and a
  trace showing `BuildAsciiCompositeSurface` in the top-3 hotspots. Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md`,
  `dev-docs/performance/investigations/performance-bottleneck-deep-dive-4.md`. (The endorsed
  *miss-path* colour-independent coverage atlas already shipped — `src/render/AsciiGlyphAtlas.{h,cpp}`
  — and is not what this guardrail forbids.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, and
the 2026-06-15/16 render/app/util/terminal closeouts — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
