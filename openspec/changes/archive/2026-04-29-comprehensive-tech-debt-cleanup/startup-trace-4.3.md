# Task 4.3 Startup Trace Evidence

Date: 2026-04-28

Representative project: `/home/user/Documents/projects/microide`

Command used for both runs:

```bash
timeout 2s env MICROIDE_STARTUP_TRACE=1 SDL_VIDEODRIVER=dummy <binary>
```

## Baseline (before 4.2)

Binary: `/tmp/microide_baseline_52069b2/build/microide/microide` at commit `52069b2`

Key load scopes:

- `WorkspaceShell::RestoreSessionState`: `0.61 ms`

Trace excerpt:

```text
[startup]    35.85 ms total |     0.61 ms |             WorkspaceShell::RestoreSessionState
```

## Current (after 4.2)

Binary: `./build/microide/microide` from working tree with 4.2 changes

Key load scopes:

- `WorkspaceShell::RestoreSessionState`: `0.23 ms`

Trace excerpt:

```text
[startup]    25.80 ms total |     0.23 ms |           WorkspaceShell::RestoreSessionState
```

## Result

`RestoreSessionState` load time improved from `0.61 ms` to `0.23 ms` in this representative run, so the structured-load path is not slower than the legacy text-format load.
