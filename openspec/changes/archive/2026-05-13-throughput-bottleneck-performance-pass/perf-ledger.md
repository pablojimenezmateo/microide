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

## After Results (Local Advisory, Isolated Harness)

Captured after harness isolation (§1), fold indexes (§2.1–§2.2), non-soft-wrap row mapping fast path (§2.3), same-line-count `ApplyHistoryEntry` fast path (§3.1), and slice-based multi-caret aggregate (§3.4). Reports in this directory:

- `perf-before-isolated-smoke.txt/json` → `perf-after-smoke.txt/json` (10 iterations)
- `perf-before-isolated-hotspots.txt/json` → `perf-after-hotspots.txt/json` (10 iterations)
- `perf-before-isolated-idle.txt/json` → `perf-after-idle-soak.txt/json` (1 iteration)
- `diff-bench-before.txt` → `diff-bench-after.txt` (5 runs)
- `search-bench-before.txt` → `search-bench-after.txt` (5 runs)

### Highest-impact deltas (p50 wall, 10-iteration isolated runs)

| Scenario | Before (ms) | After (ms) | Delta |
| --- | --- | --- | --- |
| `editor_auto_close_typing` | 3175.22 | 727.87 | **−77%** |
| `editor_smart_indent_typing` | 3246.14 | 829.45 | **−74%** |
| `editor_shaping_multi_caret` | 146.27 | 35.74 | **−76%** |
| `editor_add_cursor_next_match` | 42.87 | 25.59 | −40% |
| `editor_fold_recompute` | 1165.08 | 715.56 | **−39%** |
| `editor_bracket_match_caret_motion` | 107.56 | 69.46 | −35% |
| `editor_surround_multi_caret` | 562.88 | 485.74 | −14% |
| `editor_render_whitespace_paint` | 499.23 | 464.25 | −7% |
| `editor_indent_guides_paint` | 365.01 | 346.55 | −5% |
| `editor_mouse_selection_drag` | 232.92 | 222.81 | −4% |
| `editor_fold_viewport_refresh` | 458.79 | 497.25 | +8% (noisy, within iteration variance) |

`cold_startup_no_project` (pre-isolation max 359.71 ms → post-isolation max 24.50 ms) is the only smoke entry that moved purely from harness isolation rather than product code, but it now reflects a true cold start instead of restored 50k-line editor work.

### Adjacent paths (unchanged, as expected)

- Compare pipeline: `diff-total-ms-avg` 0.217 ms; row paint 5.28 ms; syntax 13.66 ms — same shape as `diff-bench-before.txt`.
- Search: `kernel_sized_project node_0001` literal averages 1.06 ms across 5 runs (identical to before).
- `idle_soak_30s` consumes the full 30 s budget without unexpected wakes.

Baselines under `tests/perf/baselines/*.json` SHALL NOT be moved on the basis of these advisory numbers. This change archives without a `perf-runner-v1` gate because that runner is unavailable in the current environment, and no baseline files were updated.

### Manual real-window verification (`§4.8`, 2026-05-13)

User-run command:

- `env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=5 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide`

Observed trace highlights:

- First real-window `WorkspaceRootView::Render`: **2514.65 ms** (`Application::Render(full)` total 2523.46 ms) while restoring `tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp` from `switch_project_b`.
- `RuntimeSyntaxRegistry::EnsureInitialized`: 44.07 ms.
- `TextViewport::OpenFile(...synthetic_kernel.cpp)`: 24.26 ms.
- `WorkspaceShell::InitializeCurrentProject`: 42.10 ms.
- After the initial render, repeated `Application::WorkspaceRender(fallback-full)` samples mostly settled at **14-18 ms**, with a few spikes at **27-35 ms** and occasional fast frames around **5-6 ms**.

Conclusion:

- The isolated harness work removed measurement contamination, but the real-window trace still shows a severe first-render cost on a restored 50k-line editor tab.
- Steady-state render cost is materially lower than the initial frame, but it remains borderline for smooth interaction during some redraw bursts.

## Archive Note

The authoritative `perf-runner-v1` gate (`§4.9`) was intentionally not run for this archive because GitHub/runner usage is unavailable in the current environment. The archive therefore preserves local advisory evidence only; no perf baselines were changed.
