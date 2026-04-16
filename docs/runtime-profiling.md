# Runtime Profiling

Reviewed on 2026-04-16.

MicroIDE now has two complementary profiling surfaces for post-commit regression checks:

- command-line benchmarks for repeatable before/after runs
- env-gated runtime tracing for live resize and redraw investigations

## 1. Diff Pipeline Benchmark

Build and run:

```bash
cmake --build build --target microide_diff_bench
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

## 2. Live Redraw And Resize Trace

Enable the runtime profiler with:

```bash
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 ./build/microide/microide
```

Useful variants:

```bash
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=2 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide
timeout 2s env SDL_VIDEODRIVER=dummy MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 ./build/microide/microide
```

`MICROIDE_PERF_TRACE` prints nested slow scopes to stderr. `MICROIDE_PERF_TRACE_MIN_MS`
suppresses noise below the chosen threshold. `MICROIDE_TRACE_REDRAW=1` keeps the existing
aggregate redraw summary enabled at the same time.

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
