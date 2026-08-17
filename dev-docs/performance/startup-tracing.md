# Startup Tracing

MicroIDE now has a lightweight startup tracer for measuring where launch time goes.

Use the perf harness as the primary regression gate for startup and interactive performance.
Use startup tracing as a developer fallback when you need scope-level diagnosis on a local run.

The tracer is off by default. Enable it with the `MICROIDE_STARTUP_TRACE` environment variable.

## Why It Exists

Performance priorities are:

1. startup speed
2. low CPU usage
3. low memory footprint

The tracer exists to make startup work measurable so performance changes can be driven by data rather than guesswork.

## Basic Usage

Run the app with tracing enabled:

```bash
env MICROIDE_STARTUP_TRACE=1 ./build/microide/microide
```

For a short headless smoke run:

```bash
timeout 2s env MICROIDE_STARTUP_TRACE=1 SDL_VIDEODRIVER=dummy ./build/microide/microide
```

The tracer writes timing lines to stderr.

Example output:

```text
[startup]    75.57 ms total |    72.30 ms |   WorkspaceShell::Initialize
[startup]    76.33 ms total |     0.73 ms |   TextRenderer::EnsureInitialized
[startup]    83.06 ms total |     7.47 ms | Application::FirstRender
```

## Ranked Summary

`MICROIDE_STARTUP_TRACE` streams one line per scope in the order they closed,
which is the right shape when each scope runs once. It stops being readable as
soon as a scope runs per file, per plugin, or per project entry — the same label
appears many times and nothing adds it up.

```bash
env MICROIDE_STARTUP_SUMMARY=1 ./build/microide/microide
```

prints one table instead, ranked by **self** time (a scope's own time, with
nested scopes subtracted) and carrying count / max / average per label. Both flags
compose. The columns and how to read them are documented once, in
`dev-docs/performance/runtime-profiling.md` § Ranked Scope Summary; the same
mechanism backs both channels.

The summary is emitted from `Application::Shutdown`, so a run killed with
`SIGKILL` prints nothing.

## Measuring A Real Launch End To End

The tracer streams as scopes close, so a killed process still prints. The ranked
summary does not: it is emitted from `Application::Shutdown`, and `SIGKILL` (or
`timeout`'s `SIGTERM`) prints nothing. Quit over the control channel instead —
that is the only way to get a summary from a real launch:

```bash
MICROIDE_STARTUP_SUMMARY=1 MICROIDE_PERF_SUMMARY=1 SDL_VIDEODRIVER=dummy \
  XDG_RUNTIME_DIR=/run/user/$(id -u) ./build/microide/microide <project> --control &
sleep 3
XDG_RUNTIME_DIR=/run/user/$(id -u) ./build/microide/microide control-send quit
```

Both summaries together are what you want: the startup channel gives the phases,
the perf channel gives everything else including the background threads, and its
`main ms` column separates "the user waited for this" from "a worker did this".

To compare two commits, build the other one in a worktree and **interleave** the
runs (base, head, base, head, …) rather than running nine of each. The machine
drifts; interleaving puts the drift on both sides.

## What Not To Quote

`Application::FirstRender`, `Application::Render(...)`,
`Application::PresentRetainedScene` and any other scope containing an
`SDL_RenderPresent` are **bimodal**: presents are vsync-throttled, so the number
is "did this land before or after a refresh boundary". Nine interleaved runs of
one unchanged binary read 11.2 … 16.5 ms for `FirstRender` with two modes about
4 ms apart. Its median tells you which mode the runs fell into.

What is readable:

- scopes with no present inside them (`WorkspaceShell::Initialize` and its
  children, `SdlTtfTextBackend::Initialize`, `WorkspaceShell::PrepareFrameOnce`)
- the **frame count** before the app goes idle, and which frames are full — get
  it from `MICROIDE_TRACE_REDRAW=1 MICROIDE_TRACE_REDRAW_VERBOSE=1`, which prints
  one line per frame with its reason
- syscall counts (`strace -f -c -e trace=newfstatat,getdents64,inotify_add_watch`),
  which is how the walk work in `dev-docs/performance/performance-findings.md`
  § 2026-08-15 was found and confirmed
- the counters (`MICROIDE_PERF_COUNTERS=1`), which are the only channel that
  reports whether a frame used the retained scene at all
  (`render.frames_retained` against `render.scene_fallback_frames`)

And one thing to check before trusting any launch measurement: **the display
scale.** `SDL_GetWindowDisplayScale` is 2.0 on a scaled desktop and 1.0 under
`SDL_VIDEODRIVER=dummy`, and the two took genuinely different code paths through
the render loop until 2026-08-17 (see the findings note). A number measured under
`dummy` is not a number about the app the user runs.

## How To Read It

- `total` is time since tracing started
- the middle number is the duration of that specific scope
- indentation shows nested work

When comparing runs, focus on:

- `Application::Initialize`
- `WorkspaceShell::Initialize`
- `WorkspaceShell::RestoreWorkspaceSession`
- `WorkspaceShell::InitializeCurrentProject`
- `WorkspaceShell::SetProjectRoot`
- `DirectoryTree::SetRoot`
- `DirectoryTree::RebuildEntries`
- `CollectGitStatuses`
- `FileIndex::SetRoot`
- `FileIndex::Refresh`
- `Application::FirstRender`

`FileIndex::InitialIndexReady` is the one to read for "how long until the
project is usable": it is scoped on the startup channel around the initial index
batch's apply, so its **`ms total` column** is the moment the file finder,
project search and every index-backed lookup start answering, measured from the
top of `Application::Initialize`. Its duration column is the apply alone; the
walk that feeds it is `watch::NativeSetupWalk` on the perf channel, which has no
origin and so can only say how long it took, not when it landed.

On the perf channel, the background half of a project open is:

- `watch::NativeSetupWalk` — one walk that both registers inotify watches and
  builds the initial file-index batch
- `watch::PrepareNativeBackend::CollectWatchPaths` — the PLUGIN ASSET monitor's
  walk (a small directory). Until TD-2026-08-15-252 the project tree paid this a
  second time, on its own `FileTreeWatcher`; if you see it with a four-figure
  `dirs=` count, something has re-grown a second project watcher
- `watch::PrepareNativeBackend::StartNativeBackend(dirs=N)` — the
  `inotify_add_watch` storm, which is a fraction of a millisecond next to the
  walk that feeds it

## Recommended Workflow

1. run the traced startup command
2. identify the largest scope on the critical path
3. make one targeted change
4. rerun the same command
5. compare the before/after timings

Keep the test setup stable while comparing runs:

- use the same project
- use the same saved workspace/session state
- use the same launch command

## Notes

- The tracer is intentionally env-gated so normal runs stay quiet.
- `SDL_VIDEODRIVER=dummy` is useful for repeatable smoke measurements, but real-window startup still matters for user-perceived speed.
- If a scope disappears from startup after a refactor, that is often as useful as reducing its time.
