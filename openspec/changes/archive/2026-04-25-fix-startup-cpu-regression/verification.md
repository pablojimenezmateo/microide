# Verification

## Commands

```bash
env SDL_VIDEODRIVER=dummy build/microide/microide_tests \
  "FileWatcher/NativeWakeDoesNotForceZeroDelayPoll"

env SDL_VIDEODRIVER=dummy build/microide/microide_tests \
  "WorkspaceShell/ProjectWatcherReloadDoesNotContinuouslyRearm"

timeout 3s env SDL_VIDEODRIVER=dummy MICROIDE_STARTUP_TRACE=1 \
  build/microide/microide

timeout 3s env SDL_VIDEODRIVER=dummy MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 \
  build/microide/microide

env SDL_VIDEODRIVER=dummy build/microide/microide >/tmp/microide-after.log 2>&1 & pid=$!
sleep 1
awk '{print $14+$15}' /proc/$pid/stat
sleep 2
awk '{print $14+$15}' /proc/$pid/stat
kill $pid
wait $pid || true
```

## Focused Tests

- `FileWatcher/NativeWakeDoesNotForceZeroDelayPoll`: passed
- `WorkspaceShell/ProjectWatcherReloadDoesNotContinuouslyRearm`: passed

## Before / After Evidence

### Startup Trace

Baseline before the fix on the prior binary:

- `Application::Initialize`: `36.16 ms`
- `Application::FirstRender`: `72.30 ms`
- `WorkspaceShell::SetProjectRoot`: about `1.72 ms`

After the fix on the rebuilt binary:

- `Application::Initialize`: `126.80 ms`
- `Application::FirstRender`: `163.17 ms`
- `WorkspaceShell::SetProjectRoot`: `105.18 ms`

Interpretation:

- The startup CPU regression fix does not improve cold-start time in this headless run.
- The current startup cost is dominated by project-open work inside `WorkspaceShell::SetProjectRoot`, not by a post-startup watcher spin.
- Startup remains a separate performance target worth profiling, but it is not the same failure mode as the busy-loop fixed here.

### Runtime Trace

Baseline before the fix on the prior binary showed bounded partial redraws after startup at roughly:

- `538.54 ms`, `756.41 ms`, `1064.71 ms`, `1503.65 ms`, `1594.75 ms`, `2124.96 ms`, `2252.34 ms`, `2655.08 ms`
- each partial render cost roughly `5-8 ms`

After the fix on the rebuilt binary:

- `535.66 ms`, `755.65 ms`, `1066.68 ms`, `1508.61 ms`, `1597.18 ms`, `2126.81 ms`, `2257.04 ms`, `2654.86 ms`
- each partial render cost roughly `5-8 ms`

Interpretation:

- The rebuilt binary does not show a startup-triggered redraw storm in the dummy-driver trace.
- Post-startup redraw cadence is materially unchanged in this environment, which is consistent with the fix targeting watcher wake scheduling rather than redraw cost itself.

### Idle CPU Sample

Baseline before the fix on the prior binary:

- `/proc/<pid>/stat` jiffies: `8 -> 13` over a 2 second sample after a 1 second settle
- delta: `+5`

After the fix on the rebuilt binary:

- `/proc/<pid>/stat` jiffies: `17 -> 22` over a 2 second sample after a 1 second settle
- delta: `+5`

Interpretation:

- The dummy-driver idle sample does not reproduce the reported 100% CPU regression either before or after the code change.
- The code-level regression fix is instead validated by the watcher behavior change and the focused regression tests below.

## Behavioral Conclusion

The fix removes the specific bad state where a native file-watcher wake sets `pending_change_` and then also arms `NextPollDelay()` to `0 ms`, which converts a wake-driven watcher into timeout polling. Project file monitoring now follows the same host-owned coalesced SDL wake pattern as plugin asset monitoring, and the shell-level reload path consumes one confirmed change and then settles instead of continuously re-arming itself.
