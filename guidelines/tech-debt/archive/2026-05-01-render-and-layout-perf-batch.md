# Render / review-comment / layout per-frame perf batch + sanitizer triage

- Date: 2026-05-01
- Area: rendering, workspace, terminal, testing
- Source: items §8–§12; `dev-docs/performance/performance-findings.md` (Second Performance Pass)

## Summary

Per-frame hotspots in review-comment rendering, editor pane layout, and terminal cursor reads were
indexed/cached away, and the 2026-04-29 sanitizer/fuzz triage was closed to an environment/process
follow-up only.

## Resolution

### §8 `WorkspaceReviewComments` linear scans per frame — resolved (2026-04-23)

Review comments and threads are now indexed per URI and line. `GetThreads(uri)` /
`GetComments(uri, line_index)` read from indexed URI/line buckets; the render path uses
`HasThreads(uri, line)` / `HasComments(uri, line)`; add/remove/clear invalidate affected indexes.
Code: `src/workspace/WorkspaceReviewComments.cpp`, `src/workspace/WorkspaceShellRenderFrame.cpp`
(`draw_review_comment_markers`).

### §9 `ComputeEditorPaneLayouts` called twice per render frame — resolved (2026-04-23)

Pane layout is now computed once near the top of `RenderActiveWorkspaceSurface` and reused for the
main editor render pass and scrollbar pass. Code: `src/workspace/WorkspaceShellRenderFrame.cpp`.

### §10 Terminal cursor state under three mutex locks per frame — resolved (2026-04-23)

Added `TerminalSession::CursorSnapshot()`; terminal render, caret invalidation, and pending-input
capture read row/column/visibility under one mutex acquisition. Legacy scalar accessors remain for
tests/non-hot callers. Code: `src/terminal/TerminalSession.h`,
`src/workspace/WorkspaceShellRenderBottomPanel.cpp`.

### §11 `std::find` on `marked_lines` in review-comment markers — resolved (2026-04-23)

`draw_review_comment_markers` now performs direct indexed thread/comment checks per visible line;
no per-frame marked-line vector allocation and no per-line `std::find`. Code:
`src/workspace/WorkspaceShellRenderFrame.cpp`.

### §8–12 status update (2026-05-01)

- item 8 (review-comment linear scans): resolved; indexed lookup path remains in place
- item 9 (double layout computation per frame): resolved; layout is prepared once and reused
- item 10 (terminal cursor multi-lock reads): resolved; cursor snapshots are single-lock
- item 11 (`std::find` over `marked_lines`): resolved; vector scan removed
- item 12 (sanitizer/fuzz triage tracking): reduced to environment/process follow-up only

### §12 2026-04-29 sanitizer/fuzz triage snapshot

- **§12.1 TSAN Linux prerequisite** — environment prerequisite, not an app bug: set
  `sudo sysctl vm.mmap_rnd_bits=28` before the TSAN preset. Documented in
  `dev-docs/performance/runtime-profiling.md`, `guidelines/testing.md`, `AGENTS.md`, `CLAUDE.md`.
- **§12.2 UBSAN intermittent FileWatcher assertion under heavy mixed runs** — observed once in a
  stressy mixed run; focused reruns passed. Watchlist only; promote to a dedicated item if it
  reproduces deterministically with a minimized command.
- **§12.3 Fuzz harness (PR-style short runs)** — no memory-safety findings in short runs. The
  initial clang/fuzz build surfaced integration defects (sized-delete portability in tests, missing
  object linkage in `LegacyImporterFuzz`), both fixed in-tree. No deferred fuzz finding open.
