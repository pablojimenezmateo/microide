## 1. Measurement Isolation And Clean Before Runs

- [ ] 1.1 Update `PerfHarness::InitializeDriver` to set isolated `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`, and `XDG_DATA_HOME` before workspace initialization.
- [ ] 1.2 Add deterministic cleanup for the isolated app-root before each harness process, plus a documented artifact-retention option for failed-run triage.
- [ ] 1.3 Add report metadata for runner class, SDL video driver, renderer driver, scenario list, iteration count, layout mode, seed, and advisory-vs-reference provenance.
- [ ] 1.4 Add regression coverage proving `cold_startup_no_project` ignores a real user `~/.local/state/microide/workspace-session`.
- [ ] 1.5 Update `docs/perf-harness.md` with the isolated-run contract and artifact-retention workflow.
- [ ] 1.6 Rerun clean local before reports after harness isolation and save them under this change as `perf-before-isolated-smoke.*`, `perf-before-isolated-hotspots.*`, and `perf-before-isolated-idle.*`.

## 2. Fold And Row-Mapping Hot Paths

- [ ] 2.1 Add revision-keyed fold indexes for opener-line lookup, collapsed interval lookup, and containing-range queries in `FoldingModel`.
- [ ] 2.2 Replace linear `FoldStartingAt`, `IsCollapsedAtOpener`, `IsLineHidden`, and `InnermostFoldContaining` implementations with indexed lookups while preserving existing semantics.
- [ ] 2.3 Refactor non-soft-wrap `TextViewport` row mapping so the no-collapsed-fold common path does not build one wrapped-row entry per document line.
- [ ] 2.4 Add fold-aware visible-row tests for large files, collapsed intervals before/inside/after the viewport, sticky-scroll parent lookup, and fold toggle invalidation.
- [ ] 2.5 Ensure `RenderViewModelBuilder` and `EditorViewRenderer` consume indexed fold and viewport data without render translation units doing product-state scans.
- [ ] 2.6 Run before/after local hotspot scenarios for `editor_fold_recompute`, `editor_fold_viewport_refresh`, `editor_sticky_scroll_scroll`, `editor_indent_guides_paint`, `editor_render_whitespace_paint`, `typing_large_file`, and `scroll_large_file`.

## 3. Edit And Undo Hot Paths

- [ ] 3.1 Add a same-line and same-line-count fast path in `TextViewport::ApplyHistoryEntry` that assigns affected lines in place instead of erasing and inserting through the vector tail.
- [ ] 3.2 Preserve canonical applied-edit metadata, encoding refresh, cursor restoration, visual-column updates, and derived-cache invalidation on the new fast path.
- [ ] 3.3 Add focused tests for insert, auto-close, newline, undo, redo, syntax-cache invalidation, and LSP applied-edit output on same-count history entries.
- [ ] 3.4 Replace full `document_->lines` copies in multi-caret insert, delete, backspace, and surround common paths with touched-range aggregation.
- [ ] 3.5 Replace undo-group full-buffer snapshots with child-history merge or touched-range tracking for known-range grouped edits.
- [ ] 3.6 Keep a documented conservative fallback for unknown or overlapping grouped ranges and cover it with tests.
- [ ] 3.7 Run before/after local hotspot scenarios for `editor_auto_close_typing`, `editor_smart_indent_typing`, `editor_surround_multi_caret`, `editor_mouse_selection_drag`, `editor_add_cursor_next_match`, and `editor_shaping_multi_caret`.

## 4. Verification And After Evidence

- [ ] 4.1 Run targeted unit tests for folding, text viewport history, editor essentials, retained redraw, architecture invariants, and any touched LSP applied-edit tests.
- [ ] 4.2 Run `env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/microide/microide_perf --smoke --iterations=10 --report-json=openspec/changes/throughput-bottleneck-performance-pass/perf-after-smoke.json --report-text=openspec/changes/throughput-bottleneck-performance-pass/perf-after-smoke.txt`.
- [ ] 4.3 Run selected hotspot after scenarios with the same isolated harness and save `perf-after-hotspots.json` and `perf-after-hotspots.txt`.
- [ ] 4.4 Run `idle_soak_30s` after isolation and save `perf-after-idle-soak.json` and `perf-after-idle-soak.txt`.
- [ ] 4.5 Run `./build/microide/microide_diff_bench /home/gef/Documents/projects/microide src/workspace/WorkspaceShellRenderFrame.cpp --runs=5` and save `diff-bench-after.txt`.
- [ ] 4.6 Run `./build/microide/microide_search_bench tests/perf/fixtures/kernel_sized_project node_0001 --literal --runs=5` and save `search-bench-after.txt`.
- [ ] 4.7 Capture startup and runtime after traces with `MICROIDE_STARTUP_TRACE=1`, `MICROIDE_PERF_TRACE=1`, `MICROIDE_PERF_TRACE_MIN_MS=5`, and `MICROIDE_TRACE_REDRAW=1`.
- [ ] 4.8 Ask for a real-window manual trace run if local symptoms remain: `env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=5 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide`, then resize editor, scroll a 50000-line file, type with auto-close, and run terminal long output.
- [ ] 4.9 Run the authoritative `perf-runner-v1` gate with isolated harness state before updating any `tests/perf/baselines/*.json`.
- [ ] 4.10 Update `docs/performance-findings.md` with the ranked before/after ledger, final metrics, and any remaining opportunities.
