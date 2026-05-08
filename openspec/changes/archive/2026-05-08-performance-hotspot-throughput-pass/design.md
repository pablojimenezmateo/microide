## Context

MicroIDE already enforces measurable budgets and a committed performance harness, but the current scenario set primarily protects known workflows. The repository now needs a deeper, repeatable hotspot pass that can discover previously unmeasured bottlenecks, convert findings into ranked optimization work, and close harness gaps quickly enough to prevent recurring regressions.

This change spans multiple subsystems: `tests/perf`, harness baselines, workspace/render hot paths, and architecture/performance contracts in OpenSpec. The work must preserve current policy priorities: correctness first, then speed, then low CPU usage.

## Goals / Non-Goals

**Goals:**
- Define a concrete hotspot-audit loop that uses existing perf harness infrastructure plus targeted traces for diagnosis.
- Add or upgrade perf scenarios and baseline coverage for hotspot-prone workflows that are currently under-measured.
- Produce an evidence-backed opportunity backlog with expected impact and implementation risk, so optimization work is sequenced intentionally.
- Tighten durable spec contracts so identified hotspot categories remain gated by measurable requirements.

**Non-Goals:**
- Re-architect every subsystem touched by findings in one change.
- Replace `microide_perf_tests` with a new harness technology.
- Guarantee absolute latency targets on every hardware profile outside the reference environment.

## Decisions

### Decision: Use a two-phase performance pass (discovery, then targeted optimization slices)

The pass will first collect hotspot evidence across startup, typing, scrolling, search, terminal, and idle behavior, then break optimization work into coherent implementation slices ranked by expected payoff.

- **Why:** Mixing discovery and broad refactors hides signal and makes regressions hard to attribute.
- **Alternative considered:** Immediate opportunistic edits while profiling.
  - **Rejected because:** It risks unmeasured churn and noisy baseline movement.

### Decision: Treat `microide_perf_tests` as the primary measurement source, add scenarios only where blind spots exist

Existing scenarios and baselines remain the regression oracle. New scenarios are added only when an identified hotspot category lacks deterministic harness coverage.

- **Why:** Keeps measurements CI-gateable and aligned with current performance policy.
- **Alternative considered:** Rely mostly on ad-hoc traces and profiler captures.
  - **Rejected because:** Harder to review, compare, and enforce over time.

### Decision: Record opportunities in a ranked ledger with impact and verification plan

Each hotspot opportunity must include: affected path, metric movement, expected gain, implementation complexity, and the test/baseline evidence needed for acceptance.

- **Why:** Prevents speculative optimization and ensures throughput-oriented sequencing.
- **Alternative considered:** Keep findings in informal notes.
  - **Rejected because:** Findings become stale and non-actionable.

### Decision: Convert durable findings into spec-level requirements immediately

If the audit reveals repeatable hotspot classes, the change updates `performance-harness`, `performance-budgets`, and `workspace-architecture` specs in the same pass.

- **Why:** Prevents performance discipline from depending on tribal memory.
- **Alternative considered:** Defer spec updates until all optimizations are complete.
  - **Rejected because:** Leaves a policy gap where regressions can re-enter.

## Risks / Trade-offs

- **[Risk] Added scenarios increase harness runtime** → Mitigation: keep new scenarios deterministic, bounded, and focused on high-value hot paths; place heavier runs in nightly tracks where appropriate.
- **[Risk] Baseline churn obscures real regressions** → Mitigation: require per-scenario rationale, `perf-baseline:` tags, and before/after evidence for any moved baseline.
- **[Risk] Hotspot ranking may overfit one machine profile** → Mitigation: rank by relative movement across multiple metrics (CPU, frame-time, wake-ups, RSS), not one absolute number.
- **[Risk] Optimization changes may collide with ongoing architecture cleanup** → Mitigation: sequence opportunities into small slices with explicit ownership and targeted tests.

## Migration Plan

1. Land the hotspot-audit capability and updated spec contracts.
2. Run the full perf harness plus any newly introduced scenarios and capture baseline evidence.
3. Generate and publish the ranked opportunity ledger from measured hotspots.
4. Implement top-ranked optimization slices one by one, each with targeted tests and baseline updates when required.
5. Re-run perf gate scenarios and verify no regressions in non-targeted workloads.

Rollback strategy: if new scenarios or constraints are too noisy, keep the hotspot ledger and revert only the unstable scenario/baseline additions while preserving deterministic coverage already validated.

## Open Questions

- Which currently unmeasured workflow has the highest user-facing pain: terminal burst output, large-merge scroll, or plugin diagnostics spikes?
- Should long-soak hotspot scenarios remain advisory or become merge-gating once stability is demonstrated?
- Do any hotspot classes require new lightweight in-process counters beyond existing harness metrics?
