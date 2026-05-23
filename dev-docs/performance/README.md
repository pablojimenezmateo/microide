# Performance documentation

## Primary workflow (use these first)

| Doc | Purpose |
| --- | --- |
| [`perf-harness.md`](perf-harness.md) | Scenario DSL, CI baselines, `perf-baseline:` change-record rule |
| [`startup-tracing.md`](startup-tracing.md) | Cold-start investigation (`MICROIDE_STARTUP_TRACE`) |
| [`runtime-profiling.md`](runtime-profiling.md) | Typing, scroll, redraw profiling (`MICROIDE_PERF_TRACE`, etc.) |
| [`performance-findings.md`](performance-findings.md) | Shipped wins and durable numbers worth preserving |

Authoritative policy: [`openspec/specs/performance-budgets/spec.md`](../../openspec/specs/performance-budgets/spec.md).

## Investigations (historical)

The [`investigations/`](investigations/) directory holds time-boxed bottleneck deep dives (rounds 1–4)
and one-off render notes. They remain useful for **why** a path was rejected or deferred, but they
are not the regression oracle — use the harness and specs above for gating.

When a deep dive leads to a durable decision, record the outcome in
[`../project/known-tech-debt.md`](../project/known-tech-debt.md) or `performance-findings.md`, then
treat the investigation file as read-only history.
