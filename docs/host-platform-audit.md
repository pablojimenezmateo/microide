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
| Sync subprocess | `src/platform/Subprocess.cpp` | `fork`/`execvp`, pipes, and `waitpid` used to define the whole API | explicit process service split | landed |
| Async subprocess | `src/platform/AsyncSubprocess.cpp` | long-lived child I/O used to be POSIX-only | explicit process service split | landed |
| Terminal lifecycle | `src/terminal/TerminalSession.cpp` | PTY launch, resize, shutdown, and reaping used to be coupled to rendering | terminal backend split from screen model | landed |
| File watching | `src/platform/FileWatcher.cpp` | Linux used `inotify`; other hosts used polling by default | native per-host watcher backends | landed |

## Applied Boundary Changes

- `AppDirectories` now routes host policy through `HostPlatform` so macOS and Windows app-owned
  config, state, data, and cache paths resolve correctly.
- `project/FileOperationService` now delegates trash behavior to `platform/Trash` instead of
  embedding Linux and macOS trash policy in project code.
- `WorkspaceShell::OpenExternalUrl` now delegates to `platform/HostIntegration`.
- theme and font loading now delegate to `platform/RuntimePaths`, which understands app-bundle and
  desktop-build asset layouts instead of relying on Linux-adjacent relative paths.
- `Subprocess` and `AsyncSubprocess` now provide Windows process launch and pipe handling instead
  of treating non-POSIX hosts as unsupported.
- `TerminalSession` now routes lifecycle and I/O through `platform/TerminalBackend`, keeping screen
  parsing and rendering host-agnostic while POSIX and Windows backends own launch, resize,
  shutdown, and output handling.
- `FileWatcher` now provides native Linux `inotify`, macOS `kqueue`, and Windows change
  notification backends, with polling reserved for explicit fallback cases such as missing roots
  or unsupported paths.
