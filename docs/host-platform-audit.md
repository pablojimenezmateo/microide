# Host Platform Audit

Reviewed on 2026-04-25.

This audit maps the remaining Linux-only or generic POSIX seams to the host-owned services that
should own them.

## Current Seams

| Area | Current code | Linux/POSIX assumption | Host-owned boundary | Status |
| --- | --- | --- | --- | --- |
| App directories | `src/platform/AppDirectories.*` | non-Windows hosts fell back to XDG roots | `AppDirectories` + `HostPlatform` | landed |
| Trash / recycle bin | `src/project/FileOperationService.cpp` | Linux and macOS trash logic lived in project code | `platform/Trash.*` | landed |
| Host actions | `src/workspace/WorkspaceShellTerminal.cpp` | external URL open fell back directly to `SDL_OpenURL` | `platform/HostIntegration.*` | landed |
| Runtime assets | `src/render/Theme.cpp`, `src/render/SdlTtfTextBackend.cpp` | assets were discovered through Linux-style relative paths | `platform/RuntimePaths.*` | landed |
| Sync subprocess | `src/platform/Subprocess.cpp` | `fork`/`execvp`, pipes, and `waitpid` define the whole API | explicit process service split | open |
| Async subprocess | `src/platform/AsyncSubprocess.cpp` | long-lived child I/O is still POSIX-only | explicit process service split | open |
| Terminal lifecycle | `src/terminal/TerminalSession.cpp` | PTY launch, resize, shutdown, and reaping are coupled to rendering | terminal backend split from screen model | open |
| File watching | `src/platform/FileWatcher.cpp` | Linux uses `inotify`; other hosts fall back to polling | native per-host watcher backends | open |

## Applied Boundary Changes

- `AppDirectories` now routes host policy through `HostPlatform` so macOS and Windows app-owned
  config, state, data, and cache paths resolve correctly.
- `project/FileOperationService` now delegates trash behavior to `platform/Trash` instead of
  embedding Linux and macOS trash policy in project code.
- `WorkspaceShell::OpenExternalUrl` now delegates to `platform/HostIntegration`.
- theme and font loading now delegate to `platform/RuntimePaths`, which understands app-bundle and
  desktop-build asset layouts instead of relying on Linux-adjacent relative paths.

## Remaining Bring-Up Work

- split `Subprocess` and `AsyncSubprocess` into an explicit process backend with Windows support
- split terminal lifecycle from terminal parsing/rendering so Windows and macOS backends can be
  implemented behind one host contract
- add native macOS and Windows watcher backends so supported hosts do not rely on polling by
  default
