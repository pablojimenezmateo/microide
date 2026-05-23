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
