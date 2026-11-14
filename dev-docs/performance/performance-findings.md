# MicroIDE Performance Findings

What each performance pass measured, shipped and rejected, April to September 2026:
the full record (3,700 lines) moved, verbatim, to
[`../archive/performance-findings-2026-04-to-2026-09.md`](../archive/performance-findings-2026-04-to-2026-09.md)
on 2026-11-14. Search it before re-measuring something -- a tempting change may
already have been measured and rejected there. Its April "Still Worth Doing" list is
not a backlog: open performance debt lives in
[`../project/known-tech-debt.md`](../project/known-tech-debt.md), and the experiments
that must not be retried are its "Guardrails" section.

How to measure: [`perf-harness.md`](perf-harness.md),
[`runtime-profiling.md`](runtime-profiling.md),
[`startup-tracing.md`](startup-tracing.md).

Add a new pass's findings here, newest first, as its own dated section.

## The passes in the archive, newest first

- 2026-09-20 instrumentation pass: two gates measuring a no-op, and the two O(n²) the scenario written to replace them found immediately
- 2026-08-17 compare pass: "does this have to happen?" was the wrong question
- 2026-08-17 interaction pass: three shapes, and one of them was invisible to the suite
- 2026-08-17 launch, third pass: the ignore filter was 73 % of the tree walk, and a zero said otherwise
- 2026-08-17 launch, second pass: the shell thread was waiting, and the retained scene was dead
- 2026-08-15 launch: what the shell thread does before the window is on screen
- 2026-08-13 (seventh pass) the two per-line caches free what the next frame rebuilds
- 2026-08-06 (sixth pass) opening a file you already have open (perf-runner-v1)
- 2026-08-05 (fifth pass) no caller wants the whole line (perf-runner-v1)
- 2026-08-05 (fourth pass) the rest of the long-line interactions (perf-runner-v1)
- 2026-08-05 (third pass) closing the render and folding paths on a long line (perf-runner-v1)
- 2026-08-05 (second pass) closing the edit path on a long line (perf-runner-v1)
- 2026-08-05 the edit path on a line with no newlines in it (perf-runner-v1)
- 2026-08-04 `FoldingModel` rewrite, ninth round (perf-runner-v1)
- 2026-08-04 measurement pass, eighth round (perf-runner-v1)
- 2026-08-03 measurement pass, seventh round (perf-runner-v1)
- 2026-08-01 measurement pass, sixth round (perf-runner-v1)
- 2026-08-01 measurement pass, fifth round (perf-runner-v1)
- 2026-08-01 measurement pass, fourth round (perf-runner-v1)
- 2026-08-01 measurement pass, third round (perf-runner-v1)
- 2026-08-01 measurement pass, second round (perf-runner-v1)
- 2026-08-01 measurement pass (perf-runner-v1)
- 2026-07-31 measurement pass (perf-runner-v1)
- 2026-07-04 measurement pass (perf-runner-v1)
- Fixed 2026-07-02 (compare/merge render allocations + sidebar status cache)
- Recent Optimization Pass (2026-04-22)
- Second Performance Pass (2026-04-23)
- Deep-Dive Findings (2026-04-23)
- Recent LSP Optimization Pass
- Throughput Bottleneck Performance Pass (2026-05-13)
- Plugin-Rendering Zero-Cost Verification (2026-06-26)
- Deep First-Paint Freeze Fix (2026-06-27)
- Large-file overhaul — Phase 4: direct-load fast path
- Large-file overhaul — Phase 5: incremental buffer-local find
- Large-file overhaul — Phase 6: multi-caret edit capture regression + Snapshot audit
- Compare/Merge Allocation Pass (2026-06-29)
