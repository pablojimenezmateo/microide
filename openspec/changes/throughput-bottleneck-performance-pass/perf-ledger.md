# Performance Ledger

## Evidence Captured

Local runs were captured on 2026-05-13 under `SDL_VIDEODRIVER=dummy` because this SDL build reported `x11 not available` under `xvfb-run`. Treat these as advisory ranking data until `perf-runner-v1` reruns the isolated harness.

- `perf-before-smoke.txt/json`: smoke scenarios, 3 iterations.
- `perf-before-gate-selected-3it.txt/json`: selected gate scenarios, 3 iterations.
- `perf-before-idle-soak.txt/json`: `idle_soak_30s`, 1 iteration.
- `startup-trace-before.txt`: app startup trace.
- `runtime-trace-before.txt`: runtime trace for restored-session startup.
- `editor-hotspots-trace-before.txt`: focused editor hotspot trace.
- `terminal-trace-before.txt`: terminal scenario trace.
- `diff-bench-before.txt`: compare pipeline sample.
- `search-bench-before.txt`: search benchmark sample.

## Measurement Defect

`PerfHarness::InitializeDriver` sets `XDG_CONFIG_HOME` to `/tmp/microide-perf-config`, but it does not set `XDG_STATE_HOME`. The harness therefore restored real state from `/home/gef/.local/state/microide/workspace-session`, which currently contains eight projects. This contaminates startup and terminal scenarios with unrelated project restore and 50k-line editor renders.

Observed trace evidence:

- `terminal-trace-before.txt`: `WorkspaceRootView::Render` took 1435.58 ms inside `terminal_scroll_long_output`.
- `runtime-trace-before.txt`: `WorkspaceRootView::Render` took 715.03 ms on first full render of restored 50k-line editor state.
- `startup-trace-before.txt`: `WorkspaceShell::Initialize` took 111.28 ms and `Application::FirstRender` took 164.68 ms with restored workspace state.

First implementation step: isolate `XDG_STATE_HOME`, `XDG_CACHE_HOME`, and `XDG_DATA_HOME`, then rerun clean before reports before optimizing product code.

## Ranked Product Findings

### 1. Full-document fold-aware row mapping

Evidence:

- `editor_fold_recompute`: p50 1119.56 ms, p95 1344.77 ms, p50 allocations 21296058.
- `editor_fold_viewport_refresh`: p50 429.96 ms, p95 436.38 ms, p50 allocations 12648230.
- `editor_sticky_scroll_scroll`: p50 422.76 ms, p95 434.15 ms, p50 allocations 11723873.
- `editor_indent_guides_paint`: p50 403.73 ms, p95 405.29 ms, p50 allocations 12115131.
- `editor_render_whitespace_paint`: p50 463.45 ms, p95 467.44 ms, p50 allocations 12451870.

Likely cause:

- `TextViewport::EnsureWrappedRowLayouts()` scans every document line and calls `FoldingModel::IsLineHidden()` while non-soft-wrap rendering only needs visible rows in the common case.
- `FoldingModel::{IsLineHidden,FoldStartingAt,IsCollapsedAtOpener,InnermostFoldContaining}` are linear scans over ranges.

Target:

- Indexed fold lookups and viewport-bounded non-soft-wrap row mapping.

### 2. Same-count edit and undo tail shifts

Evidence:

- `editor_auto_close_typing`: p50 3350.67 ms across 120 insert/undo iterations.
- `editor_smart_indent_typing`: p50 3207.59 ms across 120 insert/undo iterations.
- `editor-hotspots-trace-before.txt`: repeated `WorkspaceEventDispatcher::Handle::TextInput` and `TextViewport::Undo::ApplyHistoryEntry` scopes around 11-12 ms each on the 50k-line fixture.

Likely cause:

- `TextViewport::ApplyHistoryEntry()` erases and inserts lines even when the history entry replaces one line with one line, shifting the entire vector tail for edits near the top of large files.

Target:

- Same-line and same-line-count in-place assignment fast path while preserving applied-edit metadata and cache invalidation.

### 3. Multi-caret and undo-group full-buffer snapshots

Evidence:

- `editor_surround_multi_caret`: p50 520.29 ms, p95 740.99 ms, p50 allocations 17087048.
- `editor_mouse_selection_drag`: p50 229.10 ms, p95 268.81 ms, p50 allocations 8765866.

Likely cause:

- Multi-caret edit paths still copy `document_->lines` and diff whole buffers to build aggregate history.
- `BeginUndoGroup()` snapshots `document_->lines`.

Target:

- Aggregate history from touched ranges and child history entries; keep explicit conservative fallback for unknown ranges.

### 4. First-render and syntax warmup

Evidence:

- `runtime-trace-before.txt`: `RuntimeSyntaxRegistry::EnsureInitialized` 45.56 ms, `TextViewport::OpenFile` 38.15 ms, first `WorkspaceRootView::Render` 715.03 ms.
- `diff-bench-before.txt`: compare syntax average 14.705 ms with max 53.308 ms; row paint average 5.073 ms.

Target:

- Main focus remains row mapping. Syntax registry warmup is visible but below row-map/render spikes.

### 5. Adjacent paths currently lower priority

Evidence:

- `search-bench-before.txt`: literal search over `kernel_sized_project` averaged 1.06 ms.
- `search_first_result`: p50 15.50 ms, p95 31.26 ms.
- `git_sidebar_activate`: p50 75.40 ms, p95 105.00 ms.

Target:

- Keep these in adjacent verification, but do not prioritize them ahead of harness isolation and editor row/edit fixes.

## After Evidence Required

After implementation, capture:

- `perf-after-smoke.txt/json`.
- `perf-after-hotspots.txt/json`.
- `perf-after-idle-soak.txt/json`.
- `startup-trace-after.txt`.
- `runtime-trace-after.txt`.
- `editor-hotspots-trace-after.txt`.
- `terminal-trace-after.txt`.
- `diff-bench-after.txt`.
- `search-bench-after.txt`.

Baseline updates are not acceptable until the same scenario set is green or intentionally moved on `perf-runner-v1`.
