# MicroIDE Known Tech Debt

Reviewed on 2026-06-17.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
It is deliberately short. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

_None currently open._

The **timing-dependent test flakiness** item (added 2026-06-21) is now closed. The
single-check-after-a-fixed-`sleep_for` anti-pattern was audited across every
`sleep_for` site under `tests/` and remediated:

- `ControlChannelService/SocketSelfHealsAfterExternalDeletion` — poll-drains
  `ConsumeControlCallbacks()` until the descriptor reappears (commit `ae2523bb`).
- `ProjectSearchService/StopDiscardsLateUpdates` — now drains `TakePendingUpdate()`
  over a bounded window and asserts no update ever surfaces after `Stop()`, instead of
  checking once after a fixed 25 ms wait (`tests/ProjectSearchServiceTests.cpp`).
- `Subprocess` async read/write concurrency fixture — now poll-waits on an
  `std::atomic<bool> reader_running` signal so a read is genuinely in flight before the
  timed write, instead of sleeping a fixed 100 ms (`tests/SubprocessTests.cpp`).

The candidates flagged for follow-up review (`FileIndexWatcherTests`,
`ExternalRepoChangeTests`, `ProjectChangeTests`, `WorkspaceDapClientTests`,
`WorkspaceLspClientTests`, `TerminalSessionTests`) were audited and already use the
bounded poll-until-condition shape; no changes were needed. The durable convention —
**poll a condition until a bounded deadline for every cross-thread assertion; never
assert state immediately after a fixed `sleep_for`** — is recorded in
`guidelines/testing.md`.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
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
