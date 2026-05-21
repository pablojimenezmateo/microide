## Context

Existing docs list `compare_tab_open`, `merge_tab_open`, compare/merge large fixture scrolling, interleaved merge hunks, compare selection scrolling, and Git sidebar activation. They also call out remaining gaps for hunk navigation, mixed edit/scroll traces, and large merge-model build.

This change expands the harness as the regression oracle for the Git workstation wedge. It does not claim comparative performance; it adds internal budgets and evidence.

## Goals / Non-Goals

**Goals:**
- Add deterministic scenarios for the next Git/diff/merge hot paths.
- Commit baselines and tolerances for each scenario.
- Report full standard metric sets and provenance.
- Use realistic fixture repositories with fixed seeds.
- Make scenarios gate merge candidates on `perf-runner-v1`.

**Non-Goals:**
- No comparative benchmarks against other editors.
- No production feature implementation except harness hooks needed to drive scenarios.
- No periodic CI schedule changes.
- No baseline movement without `perf-baseline:` justification.

## Decisions

- Scenarios are named after user-visible workflows rather than implementation functions. This keeps budgets stable across refactors.
- Fixture repositories are generated deterministically or committed under `tests/perf/fixtures/` according to existing harness policy.
- Stage/discard scenarios may initially drive service-level seams if UI operations land later, but they must move to real UI paths before release gating.
- External-refresh scenarios simulate watcher events and assert the UI remains responsive while background work completes.

## Risks / Trade-offs

- Too many scenarios can slow local runs -> mark the full set as gate-capable while retaining a smoke subset for non-reference runners.
- Scenario flakiness can hide regressions -> fixed seeds, isolated state roots, and baseline tolerances must match existing harness rules.
- Features may not exist yet -> add placeholder-disabled scenarios only if the harness can report missing capability clearly; otherwise land scenarios with the corresponding feature change.

## Migration Plan

1. Add fixtures and scenario drivers for Git refresh and large diff/merge navigation.
2. Add staging and commit scenarios as service/UI seams become available.
3. Add external-change refresh scenarios after watcher fan-out exists.
4. Commit baselines from reference evidence and update docs.
