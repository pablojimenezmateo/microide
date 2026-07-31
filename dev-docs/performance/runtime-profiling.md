# Runtime Profiling

Reviewed on 2026-04-16.

MicroIDE now has two complementary profiling surfaces for post-commit regression checks:

- command-line benchmarks for repeatable before/after runs
- env-gated runtime tracing for live resize and redraw investigations

Perf regression gating should prefer `dev-docs/performance/perf-harness.md` and scenario baselines first. Runtime
profiling in this document is the fallback for root-cause analysis when a scenario regresses or a
new hotspot appears.

## TSAN Prerequisite (Linux)

On some Linux hosts, ThreadSanitizer fails at startup with:

`FATAL: ThreadSanitizer: unexpected memory mapping ...`

Set a lower ASLR mmap entropy value before running TSAN binaries:

```bash
sudo sysctl vm.mmap_rnd_bits=28
```

Then run TSAN tests (for example `build/microide-tsan/microide/microide_tests`).

## 1. Diff Pipeline Benchmark

Build and run:

```bash
cmake --build build --target microide_diff_bench -j8
./build/microide/microide_diff_bench /path/to/repo path/to/file --runs=5
```

The diff benchmark now measures the rewritten pipeline directly. It reports:

- read time
- full diff build time
- split-lines, line-alignment, hunk-alignment, intraline, and row-assembly stage times
- syntax tokenization time for compare rows without any large-file quality cutoff
- compare row-decoration build time
- compare row paint time on the shared decorated-row renderer
- warm editor render time for the same file
- compare and editor cache hit rates

Use it when you want a stable before/after number for compare-heavy changes.

## 2. Ranked Scope Summary (Start Here)

`MICROIDE_PERF_TRACE` streams one line per scope. That is the right tool once you know *which*
scope you care about, and the wrong one for finding it: the line is written and flushed inside the
parent scope, so the firehose perturbs the numbers it is printing, and a hot inner scope buries the
signal under thousands of lines.

For "where does the time actually go?", use the aggregating mode instead. It records nothing to
stderr during the run and prints one table at shutdown, ranked by **self** time — total wall time
inside a scope minus the time charged to scopes nested directly under it — so a cheap outer scope
cannot outrank the expensive one it merely contains:

```bash
env SDL_VIDEODRIVER=dummy MICROIDE_PERF_SUMMARY=1 ./build/microide/microide
env MICROIDE_STARTUP_SUMMARY=1 ./build/microide/microide
```

```
[perf] summary: 114 labels, 6275 calls, 355.82 ms self total (ranked by self ms)
[perf]      self ms     total ms       max ms       avg ms      calls  label
[perf]      161.063      161.063      161.063     161.0632          1  WorkspaceProjectFileMonitor::ArmPendingWatch
[perf]       85.153       85.153       15.294       3.4061         25  Application::PresentRetainedScene
[perf]       32.611       97.501       16.529       4.6429         21  Application::Render(partial)
[perf]       10.060       11.891        2.341       0.0148        802  TextViewport::HighlightedLineTokens
```

Read the columns together, not just the first one:

- **self vs total** — a large gap means the cost is in a nested scope; follow the total down.
- **max vs avg** — a `max` far above `avg` is a stall (one bad call), not a throughput problem. A
  `max` close to `avg` on a high call count is throughput, and the fix is usually to call it less.
- **calls** — the cheapest wins are here. A scope averaging 0.01 ms called 800 times per session is
  a call-count bug, not a slow function.

Both summary flags compose with the streaming flags. Turning on `MICROIDE_PERF_SUMMARY` and
`MICROIDE_PERF_TRACE` together gives the ranking plus the raw timeline, at the cost of the streaming
distortion.

Labels are capped at 4096 distinct strings per channel; anything past that folds into an
`<aggregate-overflow>` row. Labels embed paths (`TextViewport::OpenFile(path=...)`), so without the
cap a long session would grow the table without bound.

The summaries are printed from `Application::Shutdown`, not from an exit hook — the shutdown path
ends in `std::quick_exit`, which runs neither `atexit` handlers nor static destructors. A run killed
with `SIGKILL` prints nothing.

## 3. Event Counters

The `PerfCounterId` counters (`util/PerformanceCounters.h`) answer the question a sampling profiler
cannot: *how many times did this actually run?* They are one relaxed atomic add each and stay armed
in release builds.

```bash
env MICROIDE_PERF_COUNTERS=1 ./build/microide/microide
```

```
[counters] 27 of 89 counters non-zero
[counters]               2489  render.text_texture_cache_hits
[counters]                146  render.text_texture_cache_misses
[counters]                963  render.text_width_cache_queries
```

The perf harness prints per-scenario counter *deltas* for the same counters, so a scenario
regression and a live session can be compared against each other directly.

Counters are declared once in the `MICROIDE_PERF_COUNTERS` X-macro list — id and wire name in the
same row. Add new ones there; do not add a parallel name table.

## 4. Live Redraw And Resize Trace

Enable the runtime profiler with:

```bash
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 ./build/microide/microide
```

Useful variants:

```bash
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=2 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1 MICROIDE_TRACE_REDRAW_VERBOSE=1 ./build/microide/microide
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_PROJECT_EVENTS=1 ./build/microide/microide
timeout 2s env SDL_VIDEODRIVER=dummy MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 ./build/microide/microide
```

`MICROIDE_PERF_TRACE` prints nested slow scopes to stderr. `MICROIDE_PERF_TRACE_MIN_MS`
suppresses noise below the chosen threshold. `MICROIDE_TRACE_REDRAW=1` keeps the existing
aggregate redraw summary enabled at the same time.

Additional opt-in trace flags:

- `MICROIDE_TRACE_REDRAW_VERBOSE=1`
  Logs every rendered frame immediately with reason, requested mode, rendered mode, partial-to-full
  promotion, dirty-rect count, clip count, and total frame time.
- `MICROIDE_TRACE_PROJECT_EVENTS=1`
  Logs file-index watcher batches and project reload decisions, including project root, whether the
  batch was initial or incremental, batch size, whether the batch actually changed the tracked
  project index, first changed relative path, and whether the wake was caused by real file-content
  changes or index-only updates.

When a retained partial frame replays many clip rects, the trace now makes that visible in two
ways:

- `Application::WorkspaceRender(partial-loop X coalesced clip rects from Y dirty rects)` scopes
  wrap the replay loop itself
- `microide perf: partial frame replayed X coalesced clip rects from Y dirty rects` is logged when
  replay count crosses the warning threshold
- `microide perf: promoting partial frame to full redraw (...)` is logged when the app decides the
  coalesced dirty set is still too fragmented and a retained partial replay would be slower than
  one full redraw

The promotion log now reports coalesced clip count and coalesced coverage, not raw summed dirty
area. Coverage is bounded to 100%, so a value above the promotion threshold is now a real measure
of how much of the logical scene the retained redraw would touch.

During active outer-layout resize, partial replay is no longer expected:

- bottom-panel divider drags should promote directly to full redraw and may schedule bounded
  follow-up settle redraws if terminal resize changes the layout during render
- sidebar divider drags should also promote directly to full redraw while the divider is moving

If resize tracing still shows large `partial-loop ...` storms during those drag paths, treat that
as a regression in the active-resize policy rather than a threshold-tuning issue.

The redraw summary now also reports partial-frame pressure directly:

- average dirty rects per frame
- average rendered clips per frame
- maximum dirty rects seen in the sample window
- maximum rendered clips seen in the sample window

The runtime profiler now covers the resize-sensitive path explicitly, including:

- `Application::Render(...)`
- `Application::WorkspacePrepareFrame`
- retained-scene presentation updates and present
- `WorkspaceShell::PrepareRenderFrame`
- `WorkspaceShell::Render`
- `WorkspaceShell::ResizeTerminalToPanel`
- `WorkspaceShell::RenderBottomPanel`
- `EditorViewRenderer::Render`
- `WorkspaceShell::RenderCompareSurface`
- `WorkspaceShell::RenderMergeSurface`
- `TerminalSession::Resize`

The live trace now also covers the future "what caused this?" investigations that the old output
missed:

- `WorkspaceEventDispatcher::Handle::*` for mouse, keyboard, watcher, LSP, terminal, and plugin
  wake events
- `WorkspaceWakeController::HandleScheduledWake`
- `WorkspaceShell::ReloadProjectIfFilesChanged::*`
- `WorkspaceShell::StartFileIndexWatcherForCurrentProject(...)`
- `ProjectCatalogService::{ActivateProjectState,LoadProjectState,StoreCurrentProjectState}(root=...)`
- `ProjectCatalogCoordinator::{PersistActiveEntry,PersistInactiveEntriesForShutdown}(...root...)`
- `TabCoordinator::{Activate,OpenFileInNewTab}(...)`
- `TextViewport::{OpenFile,EnsureInitialHighlightState}(path=...)`
- `WorkspaceShell::{HandleMouseMotion,UpdateMouseCursor,UpdateEditorHover}`
- `WorkspaceShell::{EditorHoverTargetAtPosition,DiagnosticHoverTargetAtPosition,PluginHoverTargetAtPosition}`
- `WorkspaceShell::PluginHoverTargetForLine::QueryHover(path=...)`
- `WorkspaceShell::PrepareFrameOnce::UpdateMouseCursor`
- `WorkspaceRootView::Render::RefreshHover`
- `DirectoryTree::{Refresh,RebuildEntries,AppendDirectory}`
- `FileIndex::{ApplyBatch,EnsureFresh,RebuildCacheLocked}`
- `FileFinder::{SetIndex,Refresh,EnsureCacheBuilt}`

This is the profiler to use when the app feels slower during live resize, especially for:

- wide editor panes with many visible rows
- compare and merge surfaces
- terminal panels with large scrollback

## Recommended Regression Workflow

1. Run `microide_diff_bench` on one or two representative files before a change.
2. Repeat the same benchmark after the change and compare the stage timings, not only the total.
3. Run the app with `MICROIDE_PERF_TRACE=1` and manually resize editor and terminal layouts.
4. Compare the slowest scopes between commits. If resize regressions appear, check whether the cost
   moved into shell render, editor render, terminal resize, or retained-scene present.
5. If `Application::Render(partial)` is slow while inner render scopes stay cheap, look at the
   dirty-rect and clip-count metrics first. That usually indicates partial redraw fragmentation,
   not a single slow renderer.
6. If the app goes hot immediately after project open, inspect watcher wake policy before tuning
   render code. A native file watcher or similar wake-driven service should wake through SDL events
   and keep `NextPollDelay()` unset; a `0 ms` timeout after a native wake is a polling regression.
7. If a close or project switch stalls, look for `ProjectCatalogCoordinator::Persist...` and
   `ProjectCatalogService::LoadProjectState(root=...)` lines before touching render code.
