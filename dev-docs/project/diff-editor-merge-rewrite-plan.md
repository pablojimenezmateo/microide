# Diff and Merge Editor Rewrite Plan

Reviewed on 2026-05-23.

This is the implementation plan for the durable behavioral contract in
[`openspec/specs/diff-merge-editor/spec.md`](../../openspec/specs/diff-merge-editor/spec.md).
When this plan and the spec disagree, **the spec wins**.

## Status

| Area | State |
| --- | --- |
| Compare / merge product behavior | Shipped (tabs, hunks, merge picks, overview lane, lifecycle rules) |
| Shared row paint primitive | Partial — `editor::DecoratedTextGridRenderer` is shared; compare/merge still have dedicated render TUs |
| Unified decoration *build* pipeline | **Not done** — editor, compare, and merge still assemble row decorations through parallel paths |
| File-size diff degradation audit | **Open** — verify no silent fallbacks (see ROADMAP deferred follow-up #3) |
| Published diff/merge latency budgets | Partial — `microide_diff_bench` exists; numeric gates live in `openspec/specs/performance-budgets/spec.md` |

## Goal

One decorated text-grid pipeline across **editor**, **compare**, and **merge**:

- Row fills, syntax runs, selection, diagnostics, blame, and hunk markers share one decoration-build contract.
- Compare and merge add only surface-specific overlays (split columns, hunk chrome, merge choice markers).
- Diff semantics never degrade by file size; optimizations may trade CPU for memory only.
- Every hot-path change records `microide_diff_bench` before/after output (or documents a justified regression).

## Current Architecture (starting point)

**Models (host-agnostic, keep):**

- `src/compare/CompareModel.*` — line alignment, hunks, intraline spans
- `src/compare/MergeModel.*` — three-way hunks, choices, display rows

**Rendering (converge here):**

- `src/editor/DecoratedTextGridRenderer.*` — shared per-row paint
- `src/editor/EditorViewRenderer.*` — editor viewport uses decorated grid
- `src/workspace/WorkspaceShellCompareRender.cpp` — compare-specific layout + `CompareMergeRender`
- `src/workspace/WorkspaceShellMergeRender.cpp` — merge panes + shared helpers
- `src/workspace/CompareMergeRender.*` — compare/merge row metrics and decoration assembly

**View models:**

- `RenderViewModelBuilder` must remain the only place that decides *whether* compare/merge chrome renders;
  render TUs consume POD view models only (see `AGENTS.md` hard invariants).

## Gaps vs Spec

1. **Decoration build fork** — compare/merge still build row-decoration inputs separately from the editor path instead of one shared builder fed by surface-specific hunk metadata.
2. **Render TU duplication** — `WorkspaceShellCompareRender` and `WorkspaceShellMergeRender` duplicate editor-adjacent layout math that should shrink once decoration build is unified.
3. **Large-file correctness audit** — need a focused pass (grep + tests) for file-size thresholds that change diff algorithm, hunk visibility, or merge control availability.
4. **Measurement** — warm/cold compare-open and merge-apply expectations should be recorded in `dev-docs/performance/performance-findings.md` once reference numbers exist.

## Phased Work

### Phase A — Inventory and guardrails

- Map every call site that builds row decorations for editor vs compare vs merge.
- Add regression tests that fail if compare/merge render TUs read shell/project state directly (existing architecture lint + extend if needed).
- Run `microide_diff_bench` on a fixed fixture set; store baseline in the change record.

**Exit:** Written inventory table in the implementing OpenSpec change; bench output attached.

### Phase B — Shared decoration builder

- Introduce a narrow API (e.g. in `src/editor/` or `src/compare/`) that accepts:
  - viewport line slice,
  - syntax/diagnostic/blame inputs,
  - optional hunk/merge overlay descriptor.
- Route editor tabs through the builder first; keep pixel output identical (redraw comparison tests).
- Route compare tabs through the same builder; delete redundant decoration assembly in `CompareMergeRender` where possible.

**Exit:** Editor and compare share one decoration-build function; compare-only overlays are inputs, not a forked builder.

### Phase C — Merge panes

- Extend the builder for three-way merge display rows (`MergeDisplayModel` → decoration inputs).
- Collapse merge-specific duplication in `WorkspaceShellMergeRender.cpp` to layout + overlay only.

**Exit:** Merge panes use the same builder as compare; merge render TU shrinks measurably.

### Phase D — Correctness and performance hardening

- Complete large-file degradation audit; fix any threshold-based semantic fallbacks.
- Refresh `microide_diff_bench` baselines with `perf-baseline:` justification per `dev-docs/performance/perf-harness.md`.
- Update `dev-docs/performance/performance-findings.md` with shipped wins.

**Exit:** Spec scenarios in `diff-merge-editor` pass without known semantic exceptions; bench evidence in change record.

## Out of Scope (unless promoted)

- New compare modes or SCM integrations
- Plugin-owned compare/merge rendering
- GPU text backend or glyph-atlas experiments (see `dev-docs/project/known-tech-debt.md` item 13)

## Validation Checklist

- [ ] `microide_tests` compare/merge/redraw suites green under SDL dummy video (serial)
- [ ] `microide_diff_bench` before/after for each phase touching hot paths
- [ ] `openspec/specs/diff-merge-editor/spec.md` scenarios reviewed against shipped behavior
- [ ] `dev-docs/project/active-work.md` updated when a phase lands

## Related Docs

- [`openspec/specs/diff-merge-editor/spec.md`](../../openspec/specs/diff-merge-editor/spec.md) — authoritative requirements
- [`ROADMAP.md`](../../ROADMAP.md) — product priority for this wedge
- [`dev-docs/performance/perf-harness.md`](../performance/perf-harness.md) — regression oracle
- [`dev-docs/design/text-surface-unification.md`](../design/text-surface-unification.md) — text-input contract (orthogonal but shared editor surface)
