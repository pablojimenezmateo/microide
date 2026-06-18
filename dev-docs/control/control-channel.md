# Control channel

The control channel lets an external tool (typically an LLM) drive a running
microide instance, and lets a fresh launch open straight into a ready-to-debug
state. It is the headless counterpart to the command palette: everything routes
through the same command chokepoint, so the channel adds transport + a small
vocabulary, not a parallel control path.

## Two entry points

- **Live channel** — a per-instance AF_UNIX socket. Gated on the
  `control.enabled` setting (off by default); toggling it on starts the listener
  immediately (no restart), because the SDL wake event is always registered and
  only the listener is gated (`WorkspaceShell::MaybeStartControlChannel`).
- **Cold-start spec** — `microide --control-spec <file.json>` opens a project
  with breakpoints already set (and optionally files revealed / a session
  started) before the window is interactive. The spec's `project` field selects
  the project; the spec is translated to command lines and applied via
  `WorkspaceShell::ApplyControlSpec`.

## Architecture

```
client ──JSONL──▶ ControlSocketServer (platform, 1 I/O thread)
                     │ inbound queue + SDL wake          ▲ outbound / broadcast
                     ▼                                   │
                  ControlChannelService (main thread) ───┘
                     ├─ command → execute_command_line → CommandPromptCoordinator
                     ├─ query   → read WorkspaceContext → JSON
                     └─ debug hooks → broadcast stopped/terminated/output events
```

- `src/platform/ControlSocketServer.{h,cpp}` — single-threaded poll loop over the
  listen fd, a wake pipe, and every client fd. Inbound lines are queued for the
  main thread; replies/broadcasts are queued from the main thread and flushed by
  the I/O thread. Mirrors the DAP client's marshaling discipline: the background
  thread never touches host state.
- `src/workspace/ControlChannelService.{h,cpp}` — owns the server, drains inbound
  requests on the control SDL wake event (`ConsumeControlCallbacks`), dispatches
  commands through the `Operations.execute_command_line` seam, answers queries by
  reading `WorkspaceContext`, and broadcasts debug events (wired from the
  `DebugService` operations in `WorkspaceShellPlugins.cpp`).
- `src/workspace/ControlProtocol.{h,cpp}` — request/response/event (de)serialize
  plus `ControlChannelHelpText()`, the single source of truth for the protocol
  text rendered by `microide control-help` and the man page.
- `src/workspace/ControlSpec.{h,cpp}` — parse the cold-start spec and translate
  it to command lines (`QuoteCommandArg` keeps arguments `ParseCommandLine`-safe).
- `src/workspace/WorkspaceShellControl.cpp` — thin shell forwarders.

## Protocol

Newline-delimited JSON, one object per line.

| Direction | Shape |
|-----------|-------|
| request (command) | `{"id":1,"command":"breakpoint-set src/main.cpp 42"}` |
| request (query)   | `{"id":2,"query":"debug-state"}` |
| response (command)| `{"id":1,"ok":true,"feedback":"..."}` / `{"id":1,"ok":false,"error":"..."}` |
| response (query)  | `{"id":2,"ok":true,"result":{...}}` |
| event             | `{"event":"stopped","file":"x.py","line":42,"reason":"breakpoint","frames":[...]}` |

Query verbs: `debug-state`, `breakpoints`, `tabs`, `projects`, `status`.
Events: `stopped`, `terminated`, `output`.

Line numbers are 1-based on every developer-facing surface (commands, spec,
events, queries); the single 1-based→0-based conversion lives in the
breakpoint-command executor (`WorkspaceGlobalActionExecutor.cpp`).

## Breakpoint commands

`breakpoint-set`, `breakpoint-remove`, `breakpoint-enable`, `breakpoint-disable`,
`breakpoint-condition`, `breakpoint-hit-condition`, `breakpoint-logmessage`,
`breakpoint-clear`, plus `debug-launch [name]`. All take an explicit
`<file> <line>` (unlike the context-menu breakpoint modifiers, which read the
gutter line), are gated on `debug.enabled`, and re-send to a live session via
`DebugService::ResendBreakpointsForFile`. Breakpoints placed this way persist
through the normal `PersistedDebugState` path.

## Discovery & security

Socket: `$XDG_RUNTIME_DIR/microide/<pid>.sock` (0600). Descriptor:
`$XDG_RUNTIME_DIR/microide/instances/<pid>.json` with `{pid, socket,
project_root, project_hash}`. `microide control-list` enumerates them. The
channel grants command-palette-level power, so it is opt-in per instance and the
socket is user-private.
