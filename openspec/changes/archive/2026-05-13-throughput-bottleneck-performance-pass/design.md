## Context

The local evidence for this change is stored next to the proposal:

- `perf-before-smoke.txt/json`: dummy-driver smoke run, 3 iterations.
- `perf-before-gate-selected-3it.txt/json`: selected non-smoke run, 3 iterations.
- `perf-before-idle-soak.txt/json`: `idle_soak_30s`, 1 iteration.
- `startup-trace-before.txt` and `runtime-trace-before.txt`: startup/runtime traces.
- `editor-hotspots-trace-before.txt`: focused trace for fold recompute, auto-close typing, smart indent, and surround multi-caret.
- `terminal-trace-before.txt`: terminal scenario trace showing restored-editor contamination.
- `diff-bench-before.txt` and `search-bench-before.txt`: compare/search reference samples.

The measurements identify two classes of work. First, the harness is not isolated enough: `PerfHarness::InitializeDriver` sets `XDG_CONFIG_HOME`, but not `XDG_STATE_HOME`, `XDG_CACHE_HOME`, or `XDG_DATA_HOME`, so every scenario can restore real user workspace state. Second, once the noisy harness state is understood, real editor costs remain: 50k-line render paths rebuild full row maps, fold lookups are linear in range count, and small edits near the top of a large buffer spend about 11-12 ms per apply/undo in vector erase/insert and cache invalidation.

## Goals / Non-Goals

**Goals:**

- Make local and reference perf runs reproducible by isolating all app directories used by the harness.
- Produce a ranked hotspot ledger with before and after report paths, not just narrative claims.
- Reduce first-render and block-structure scroll cost on 50000-line fixtures by making fold-aware row mapping indexed and viewport-bounded.
- Reduce single-range edit and undo latency on large files by fast-pathing same-line/same-count history application.
- Remove remaining full-buffer snapshots from multi-caret and grouped edit paths when the affected ranges are known.
- Verify every optimization with before and after harness reports and targeted traces.

**Non-Goals:**

- Do not replace the text storage container with a rope or piece table in this pass. That may be warranted later, but the measured costs have smaller high-impact fixes first.
- Do not retune baselines until the harness is isolated and `perf-runner-v1` evidence exists.
- Do not optimize compare/search first. The captured samples show compare row paint and literal search are not the dominant current bottlenecks.
- Do not add a GPU requirement or renderer dependency.

## Decisions

### Isolate Harness State Before Optimizing

`microide_perf` SHALL create a unique app-root sandbox for each harness process, set `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`, and `XDG_DATA_HOME`, and clean it before scenarios run. A `--keep-artifacts` style debug option can preserve the directory when triaging a failure.

Rationale: without this, `cold_startup_no_project` and `terminal_scroll_long_output` can measure restored project/session work from the developer machine. The terminal trace captured a 1435 ms `WorkspaceRootView::Render` from a restored 50k-line editor during the terminal scenario.

Alternative considered: manually delete `~/.local/state/microide` before runs. Rejected because it is destructive to user state and cannot be a CI contract.

### Make Fold-Aware Row Mapping Indexed

`FoldingModel` should maintain derived indexes when ranges or collapsed flags change:

- opener line to range index for `FoldStartingAt` and `IsCollapsedAtOpener`.
- collapsed interval representation for `IsLineHidden`.
- containing-range query support for sticky scroll and active indent guide lookup.

`TextViewport` should avoid rebuilding `wrapped_row_layouts_` for every document line when soft wrap is disabled and no collapsed folds affect the requested viewport. For non-soft-wrap, the common map is line-index equals visual-row-index; collapsed folds can be represented by skip intervals and prefix counts. Soft-wrap remains broader but should still reuse per-line width and row-count caches.

Rationale: first full renders exceeded 900 ms on a restored 50k-line editor, and `EnsureWrappedRowLayouts` currently loops every line and calls linear fold-hidden checks.

Alternative considered: cache the current full `wrapped_row_layouts_` more aggressively. Rejected because edit and fold revisions legitimately invalidate it, and rebuilding the whole map is the core cost.

### Fast-Path History Application For Same-Count Edits

`TextViewport::ApplyHistoryEntry` should assign in place when `removed_count == inserted_lines.size()`, especially for single-line replacements from ordinary typing and undo. It should only use vector erase/insert when line count changes.

Rationale: targeted tracing shows `WorkspaceEventDispatcher::Handle::TextInput` and `TextViewport::Undo::ApplyHistoryEntry` each spending about 11-12 ms per operation on the 50k-line fixture. Ordinary auto-close typing replaces one line with one line; shifting the entire vector tail is unnecessary.

Alternative considered: replace `std::vector<std::string>` storage now. Rejected as larger than needed for this pass.

### Build Multi-Caret History From Ranges, Not Full Snapshots

Multi-caret insert/delete/backspace and surround paths should collect affected ranges and build one aggregate `HistoryEntry` from those ranges rather than copying `document_->lines` and diffing the whole buffer. Undo grouping should similarly store touched ranges or merge child history entries instead of snapshotting the full buffer in `BeginUndoGroup`.

Rationale: static inspection still shows full `document_->lines` copies in multi-caret paths and undo groups, and `editor_surround_multi_caret` allocates tens of millions of objects locally.

Alternative considered: keep the snapshot and rely on allocator tuning. Rejected because it violates the existing performance invariant and scales with total file size rather than edit size.

### Verification Matrix

The implementation must collect both local evidence and reference-runner evidence:

- Before and after: `microide_perf --smoke --iterations=10 --report-json=... --report-text=...`.
- Before and after: selected hotspot scenarios `editor_fold_recompute,editor_fold_viewport_refresh,editor_sticky_scroll_scroll,editor_indent_guides_paint,editor_render_whitespace_paint,editor_auto_close_typing,editor_smart_indent_typing,editor_surround_multi_caret,editor_mouse_selection_drag,terminal_scroll_long_output,idle_soak_30s`.
- Before and after traces: `MICROIDE_STARTUP_TRACE=1`, `MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=5`, and `MICROIDE_TRACE_REDRAW=1` for the editor hotspot scenarios.
- Adjacent path checks: `diff-bench`, `search-bench`, `git_sidebar_activate`, and `search_first_result`.

Local dummy-driver runs are useful for ranking and regression triage but must be labelled as local. `perf-runner-v1` remains authoritative for baseline updates.

## Risks / Trade-offs

- Harness isolation changes baseline numbers because stale restored state disappears. Mitigation: land isolation first, regenerate baselines only from `perf-runner-v1`, and tag baseline moves with `perf-baseline:`.
- Fold-row indexes can go stale after edits, fold toggles, or language changes. Mitigation: key indexes on fold revision and layout revision, add focused tests for edit, collapse, expand, and language-switch invalidation.
- Same-count history application might miss cache invalidation for line content changes. Mitigation: preserve `InvalidateDerivedCaches(start_line)`, visual-column cache updates, encoding refresh, and last-applied-edit behavior; add tests for undo/redo and syntax cache invalidation.
- Multi-caret aggregate history is more complex than full-buffer diffing. Mitigation: keep a conservative fallback for overlapping or hard-to-normalize ranges, but assert the common non-overlapping path avoids full snapshots.
- Dummy SDL measurements may differ from Xvfb/reference runs. Mitigation: record local data as advisory and require reference evidence before closing the change.
