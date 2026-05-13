## Why

Local perf runs show that the current bottleneck picture is mixed with measurement noise: `microide_perf` isolates config but still restores real user state from `~/.local/state/microide`, so "cold" and terminal scenarios can pay for an unrelated restored 50k-line editor render. After accounting for that, the strongest real opportunities are in large-buffer editor row mapping, small edit/undo application, multi-caret snapshotting, and block-structure render allocations.

This pass is needed now because MicroIDE's product goal is speed first, and the current harness can both hide regressions and attribute costs to the wrong subsystem.

## What Changes

- Make the performance harness isolate config, state, cache, and data roots per run or per scenario, with deterministic cleanup and optional artifact retention for debugging.
- Add a performance audit ledger for the measured before data already captured under this change, including the local-run limitations and the exact commands needed for reference-runner before/after validation.
- Optimize large-buffer editor rendering by replacing full-document wrapped-row and folded-line scans with indexed, viewport-bounded row mapping.
- Optimize small editor edits and undo/redo by avoiding vector erase/insert tail shifts when a history entry replaces the same number of lines.
- Remove remaining full-buffer snapshots from multi-caret edits and undo grouping where the affected ranges are known.
- Keep compare/search/git coverage in the verification set, but prioritize editor and harness fixes because the measured diff and search samples are not the current top bottlenecks.
- Require after-runs to include the same harness reports and targeted traces as the before set before any implementation is considered complete.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `performance-harness`: require full state isolation, clean per-scenario startup, and report metadata that distinguishes reference-runner evidence from local dummy-driver evidence.
- `performance-hotspot-audit`: require the audit ledger to include raw before and after report paths, ranked findings, confidence level, and explicit follow-up commands.
- `editor-block-structure-affordances`: tighten the performance contract for fold-aware visible-row mapping, sticky scroll, fold gutter lookups, indent guides, and render whitespace on 50000-line fixtures.
- `editor-edit-delta-pipeline`: require single-range edits, undo, redo, multi-caret aggregate edits, and undo groups to avoid whole-buffer snapshots or full-vector tail shifts when the changed range is known.

## Impact

- Affected code: `tests/perf/PerfHarness.*`, `tests/perf/PerfMain.cpp`, `tests/perf/baselines/*`, `src/editor/TextViewport.*`, `src/editor/FoldingModel.*`, `src/editor/EditorViewRenderer.cpp`, `src/workspace/RenderViewModelBuilder.*`, and focused regression tests for editor edit deltas and folding row maps.
- Affected docs: `docs/perf-harness.md`, `docs/performance-findings.md`, and any baseline-change commit or PR text must include `perf-baseline:` when committed baselines move.
- Measurement artifacts already captured for this proposal: `perf-before-smoke.json`, `perf-before-gate-selected-3it.json`, `perf-before-idle-soak.json`, `startup-trace-before.txt`, `runtime-trace-before.txt`, `editor-hotspots-trace-before.txt`, `terminal-trace-before.txt`, `diff-bench-before.txt`, and `search-bench-before.txt`.
- No external dependencies are required.
