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

The kernel's ASLR entropy is above what TSAN's shadow mapping assumes. Clear ASLR for the one
process instead of lowering it for the whole machine — no root required:

```bash
setarch -R env TSAN_OPTIONS=suppressions=tests/tsan.supp \
  ./build/microide-tsan/microide/microide_tests
```

`setarch -R` calls `personality(ADDR_NO_RANDOMIZE)` on the child, so the setting dies with the
process and affects nothing else. `tools/run-checks.sh tsan` already does this (and already exports
the suppressions); prefer it over driving the binary by hand.

The suppressions file is not optional: without it a libdbus lock-order inversion and a Mesa
`libgallium` race in the third-party stack report as failures that have nothing to do with this
codebase.

Fallback for sandboxes that block that personality bit — machine-wide, needs root, persists until
reboot:

```bash
sudo sysctl vm.mmap_rnd_bits=28
```

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
[perf] summary: 123 labels, 6366 calls, 500.76 ms self total, 145.27 ms on the main thread
[perf]      self ms      main ms     total ms       max ms       avg ms      calls  label
[perf]      175.755        0.000      175.755      175.755     175.7552          1  WorkspaceProjectFileMonitor::ArmPendingWatch
[perf]      169.109        0.000      169.109       35.410       8.4554         20  platform::RunSubprocess(program=git)
[perf]       71.640       71.640       71.640       15.723       2.8656         25  Application::PresentRetainedScene
[perf]       15.856       15.856       53.620       16.566       2.5533         21  Application::Render(partial)
```

Read the columns together, not just the first one:

- **main vs self** — read this one first. Self time ranks CPU cost; main time ranks *what the user
  waits on*. In the run above the two biggest CPU consumers (a background tree walk and 20 git
  subprocesses, 345 ms combined) cost zero frames, and the real interactive cost is 145 ms
  dominated by present. Optimizing the top of the self-time table would have been wasted work. A
  second table at the bottom of the dump re-ranks by main ms alone.
- **self vs total** — a large gap means the cost is in a nested scope; follow the total down.
- **max vs avg** — a `max` far above `avg` is a stall (one bad call), not a throughput problem. A
  `max` close to `avg` on a high call count is throughput, and the fix is usually to call it less.
- **calls** — the cheapest wins are here. A scope averaging 0.01 ms called 800 times per session is
  a call-count bug, not a slow function.

The main-thread column depends on someone calling `util::MarkTracingMainThread()` — `main()` does,
in the app, the test binary, and the perf harness. If a new entry point does not, the summary prints
a note saying the column is meaningless rather than letting you read zeros as "nothing runs on the
main thread".

Both summary flags compose with the streaming flags. Turning on `MICROIDE_PERF_SUMMARY` and
`MICROIDE_PERF_TRACE` together gives the ranking plus the raw timeline, at the cost of the streaming
distortion.

Labels are capped at 4096 distinct strings per channel; anything past that folds into an
`<aggregate-overflow>` row. Labels embed paths (`TextViewport::OpenFile(path=...)`), so without the
cap a long session would grow the table without bound.

The summaries are printed from `Application::Shutdown`, not from an exit hook — the shutdown path
ends in `std::quick_exit`, which runs neither `atexit` handlers nor static destructors. A run killed
with `SIGKILL` prints nothing.

### From the perf harness

`microide_perf` honours `MICROIDE_PERF_SUMMARY` too, and this is usually the way to reach the
ranking: a scenario is a fixed, repeatable workload, where a hand-driven session is not.

```bash
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MICROIDE_PERF_SUMMARY=1 \
  ./build/microide-perf-make/microide/microide_perf --scenarios=<name> --iterations=3
```

The table is scoped to **one scenario's measured iterations**: the aggregate is reset after warmup,
whose one-time cold work would otherwise dominate every row, and written before the next scenario
starts. Running several scenarios in one invocation therefore gives one table each rather than a
blend.

Note the harness has no `Application::Shutdown` to print from — it writes the table itself, per
scenario. Before that existed the env var silently did nothing here.

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

Four of them exist to make a SILENT DEGRADATION audible rather than to profile
anything, and are worth reading on any launch:

| counter | what a healthy run reads |
| --- | --- |
| `render.frames_retained` / `render.scene_fallback_frames` | fallback at or near zero outside a resize drag. It sat at 100% of frames on every HiDPI display until 2026-08-17, with the entire partial-redraw path dead and nothing else reporting it |
| `watch.file_index_watcher_starts` | exactly one per project open or switch. A second on the same project is a full tree walk and one `inotify_add_watch` per directory, again |
| `watch.file_index_refresh_requests` | zero on a launch. A whole-tree rescan at startup is one the project open already did |

That is the general shape to copy when fixing a defect that hid in a legitimate
fallback: the fix is not finished until the degradation has a number.

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
