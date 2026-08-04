# Performance Guide

Purpose: define the durable performance expectations and measurement workflow for `microide`.

## Quick Scan

- Correctness comes first, but speed is the primary optimization target after that.
- CPU usage matters more than memory usage, especially on idle and redraw paths.
- Measure before and after performance-sensitive changes.
- Protect startup, typing, scrolling, resize, and redraw responsiveness.
- Prefer deleting redundant work over adding speculative caching.

## When To Measure

Collect evidence when work touches:

- startup or bootstrap paths
- redraw invalidation or render composition
- text layout, syntax highlighting, or editor scrolling
- project search, indexing, compare, or merge paths
- terminal rendering or buffer handling
- plugin loading, reload, or contribution dispatch

## Tools And Entry Points

- Use `dev-docs/performance/perf-harness.md` as the primary performance regression oracle.
- Run smoke scenarios locally through `microide_perf_tests` and treat `perf-runner-v1` baseline checks as the merge gate for perf-sensitive changes.
- Use `dev-docs/performance/startup-tracing.md` for startup investigation.
- Use `dev-docs/performance/runtime-profiling.md` for runtime and redraw profiling.
- Use the in-tree benchmark utilities when search or diff behavior changes.
- Keep before-and-after notes when a change claims a performance improvement.

To find *which* code to look at, start with the ranked summary rather than the
per-scope stream:

```bash
env MICROIDE_PERF_SUMMARY=1 MICROIDE_PERF_COUNTERS=1 ./build/microide/microide
```

It prints one table at shutdown ranked by self time, plus a second ranked by
main-thread time only. **Read the main-thread column first.** Self time ranks CPU
cost; main time ranks what the user waits on, and they routinely disagree — a
background tree walk and 20 git subprocesses were the top two rows of a real
startup summary and cost zero frames between them. Optimizing the top of the
self-time table would have been wasted work.

## Adding Instrumentation

- New scopes go through `util::PerformanceTrace::Scope`; new event counters go in
  the `MICROIDE_PERF_COUNTERS` X-macro list in `util/PerformanceCounters.h`
  (id and wire name in one row — do not add a parallel name table).
- Build a label that carries the path/index it ran on with
  `PerformanceTrace::ScopeLabel`, never a hand-rolled `if (Enabled())` guard
  around string concatenation. A missed guard is a heap allocation per call in
  production.
- Scope at the granularity the cost actually has. Per chunk, per request, per
  build — not per byte, per line, or per glyph. A counter on a path that runs
  millions of times costs more than it can reveal, which is why the piece tree's
  `LineView` is deliberately uninstrumented.
- Label by the stable part, not the operands: `git::RunCommand(sub=status)`, not
  the full argv. A label minted per invocation blows the 4096-label cap and turns
  the ranked table back into a log.
- If a subsystem already measures itself in a shape a scope cannot wrap, feed
  the number in with `PerformanceTrace::RecordSampleNs` instead of adding a
  second measurement.
- Instrumenting a file adds a link dependency on the tracer. Bench and fuzz
  targets list their sources by hand; append `MICROIDE_INSTRUMENTATION_SOURCES`
  rather than naming the util files individually (fuzz targets get it
  automatically). Fuzz-target link breaks are silent — no default build flow
  compiles them.

## Redraw And Interaction Rules

- Treat typing, scrolling, mouse movement, and resize paths as latency-sensitive.
- Keep dirty-region invalidation explicit and reviewable.
- Avoid hidden full-frame repaint regressions.
- Be careful when adding work to hover, caret, or animation-related paths.
- If a feature adds background work, verify that it does not create idle CPU churn or unnecessary wakeups.

## Optimization Heuristics

- Remove redundant work before reaching for caches.
- Keep caches scoped, invalidated explicitly, and justified by measured wins.
- Push expensive integration work off hot UI paths when that does not compromise correctness.
- Prefer focused data-flow cleanup over broad cleverness that obscures ownership.

## Reading A Measurement

Most reported regressions are not regressions. Before spending time on a number,
apply the triage in `dev-docs/performance/perf-harness.md` § Reading A Measurement:
re-run anything flagged at `ITERATIONS=25`, treat identical allocation counts on
both sides as proof no extra work is being done, and check the machine was quiet.
`max_wall_ms`-only movement with a flat p50 is a single sample.

## Validation

- For changes that can affect startup, typing, scroll, project switch, search, compare/merge, or idle behavior, run targeted perf-harness scenarios and compare baseline deltas.
- Compare measurements before and after the change.
- Call out tradeoffs when a correctness fix knowingly costs performance.
- Update docs when the performance workflow, benchmark expectations, or preserved constraints materially change.

## Durable Budget Contract

The authoritative latency, CPU, idle, and measured-before-merged policy lives in
`openspec/specs/performance-budgets/spec.md`. This guide explains the workflow; the spec defines
what changes must prove before merge.
