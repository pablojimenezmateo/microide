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

- Use `docs/startup-tracing.md` for startup investigation.
- Use `docs/runtime-profiling.md` for runtime and redraw profiling.
- Use the in-tree benchmark utilities when search or diff behavior changes.
- Keep before-and-after notes when a change claims a performance improvement.

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

## Validation

- Compare measurements before and after the change.
- Call out tradeoffs when a correctness fix knowingly costs performance.
- Update docs when the performance workflow, benchmark expectations, or preserved constraints materially change.

## Durable Budget Contract

The authoritative latency, CPU, idle, and measured-before-merged policy lives in
`openspec/specs/performance-budgets/spec.md`. This guide explains the workflow; the spec defines
what changes must prove before merge.
