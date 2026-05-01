## Why

The previous performance pass eliminated the worst render-path and plugin-reload hotspots, establishing a solid baseline. What remains are five categories of work where the main thread still blocks on external I/O or OS calls, where amortizable work is recomputed from scratch every frame, and where the user sees a blank or stale surface while avoidable work completes. The goal of this pass is to ensure that every operation the user perceives as "instant" actually is instant, and that the CPU is provably idle when the user is idle — the same principle behind high-throughput output generation: precompute once, amortize across frames or events, and never stall the hot path waiting for slow work.

## What Changes

- **Event-driven project file index**: replace the snapshot-based project file scan (triggered on file-finder open and search start) with a platform-native, always-current in-process index maintained by `inotify` (Linux), `FSEvents` (macOS), and `ReadDirectoryChangesW` (Windows). File finder and search consume the index directly with zero scan latency.
- **Background subprocess isolation**: move all git subprocess calls (status, blame, log) fully off the main thread; deliver results via SDL wake event. Add an architectural invariant ensuring that no future code path can block the main thread waiting on subprocess I/O or LSP socket reads. Lsp `textDocument/didOpen` delivery is verified non-blocking by a new lint rule.
- **Layout and geometry caching**: cache `WorkspaceLayout` with a dirty flag; pre-compute the visible-line range once per frame in `PrepareFrameOnce` and propagate it to all render phases; cache tab-strip geometry per geometry-key. Eliminate all redundant per-frame recomputation.
- **Incremental search result streaming**: enable PCRE2 JIT compilation (compiled once per pattern, cached); the search worker delivers result batches to the UI incrementally via wake event so the first results appear before the full corpus is scanned.
- **Adaptive idle rendering**: when no input is pending, no caret is active, and no background work is in flight, park the SDL event loop on a wait with no artificial zero-delay wakeups; when only a caret blink is pending, use the blink period as the sole wake timeout. Near-zero CPU at true idle, confirmed by the `idle_soak_30s` harness scenario.

## Capabilities

### New Capabilities

- `event-driven-file-index`: always-current in-process project file index backed by platform-native file-system events; consumed by file finder, project search, and diagnostics with no scan on demand
- `background-subprocess-isolation`: all git and LSP subprocess I/O runs on background threads with results delivered via SDL wake event; architectural lint enforcement that main thread never blocks on process I/O
- `layout-geometry-cache`: WorkspaceLayout cached with dirty tracking; visible-line range pre-computed once per frame; tab-strip geometry cached per geometry-key
- `incremental-search-streaming`: PCRE2 JIT enabled and cached per pattern; search result batches streamed to UI as they arrive
- `adaptive-idle-rendering`: SDL event loop parks on wait at true idle; caret-blink period drives the sole wake timeout when only caret animation is pending

### Modified Capabilities

- `performance-budgets`: add scenarios for file-finder open latency (≤ 50 ms cold), git sidebar first-paint after activation (≤ 200 ms), search time-to-first-result (≤ 100 ms on a 10 000-file project), and idle CPU (near-zero after 30 s with no input)
- `performance-harness`: add `file_finder_cold`, `git_sidebar_activate`, `search_first_result`, and `idle_soak_30s` scenarios to the automated harness

## Impact

- **src/platform/**: new `FileIndexWatcher` service wrapping `inotify`/`FSEvents`/`ReadDirectoryChangesW`; existing `ProcessBackend` extended with non-blocking result delivery
- **src/project/**: `FileIndex` refactored to consume watcher events instead of triggering scans; `GitOperations` moved to background-thread dispatch
- **src/workspace/**: `WorkspaceShellRenderFrame` and `PrepareFrameOnce` updated for visible-line-range propagation and layout caching; `WorkspaceShellRender` updated for adaptive idle path
- **src/search/**: PCRE2 JIT compilation enabled at engine init; search worker refactored to stream batches
- **tests/**: new lint rules for subprocess-on-main-thread detection; new harness scenarios; updated baselines
- **No GPU dependency**: all changes use CPU-side caching, background threads, and SDL wake events only
- **Cross-platform**: file-watching uses platform-native APIs with a poll fallback identical to the existing `FileWatcher` pattern; all background threads use `std::thread` with SDL wake events for delivery
