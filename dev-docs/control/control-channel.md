# Control channel

The control channel lets an external tool (typically an LLM) drive a running
microide instance, and lets a fresh launch open straight into a ready-to-debug
state. It is the headless counterpart to the command palette: everything routes
through the same command chokepoint, so the channel adds transport + a small
vocabulary, not a parallel control path.

## Entry points

- **Headless `--control`** — `microide --control` force-starts the channel
  (bypassing the `control.enabled` gate) *and* mirrors every response, event, and
  cold-start `applied` line to **stdout as JSONL** (`WorkspaceShell::
  ForceStartControlChannel` → `ControlChannelService::SetStdoutMirror`). This is
  the entry point for an LLM/agent: it can drive the instance and observe results
  from one stdout stream, with or without ever opening the socket. The real window
  stays running and fully interactive. Because `--control` owns the channel
  lifecycle for the whole run, a live settings change (e.g. a spec's `set-setting
  debug.enabled true`) never tears the socket down — `MaybeStartControlChannel`
  early-returns when `control_stdout` is set, so the advertised socket persists.
- **Live channel** — a per-instance AF_UNIX socket. Gated on the
  `control.enabled` setting (off by default); toggling it on starts the listener
  immediately (no restart), because the SDL wake event is always registered and
  only the listener is gated (`WorkspaceShell::MaybeStartControlChannel`). It can
  also be started transiently with `microide --set control.enabled true` (the
  override is never persisted — see below).
- **Cold-start spec** — `microide --control-spec <file.json>` opens a project
  with breakpoints already set (and optionally settings applied, files revealed, a
  session started) before the window is interactive. The spec's `project` field
  selects the project (and wins over a restored session); the spec is translated
  to command lines and applied via `WorkspaceShell::ApplyControlSpec`, which emits
  one `{"applied":...}` JSONL line per entry.

## Deterministic settings (`set-setting`, `--set`, spec `settings`)

- `set-setting <id> <value>` is a normal command (palette / socket / spec): it
  routes through the `SetSettingValue` chokepoint and **persists** like the
  Settings overlay. Unknown id / invalid value rejects with `ok:false`.
- `--set <id> <value>` (repeatable) and the spec `settings` key apply the same
  values **transiently**: live this session but never written to the saved config.
  They are tracked in `WorkspaceContext::transient_setting_keys` and stripped by
  the persistence coordinator before `SaveUserConfig`/`SaveConfigState` serialize,
  so a headless drive can flip `control.enabled`/`debug.enabled` without clobbering
  the user's config. The debugger also auto-enables (transiently) when a spec
  carries breakpoints or a launch, removing the old ordering trap.

## Headless agent runbook

```bash
# Drive an instance and stream results as JSONL on stdout:
microide --control --control-spec /tmp/debug.spec.json
```

Spec (`/tmp/debug.spec.json`):

```json
{
  "project": "/path/to/project",
  "breakpoints": [{"file": "src/main.cpp", "line": 42}],
  "open": ["src/main.cpp"],
  "launch": "My Launch Config"
}
```

The stdout JSONL stream, in order:

1. `{"event":"ready","pid":..,"socket":"..","project_root":".."}` — handshake.
2. one `{"applied":"<command>","ok":true|false,"error":".."}` per spec entry
   (settings, auto-enable, breakpoints, opens, launch, raw commands).
3. `{"event":"output",...}` / `{"event":"stopped",...}` /
   `{"event":"terminated",...}` as the session runs. Each stop emits the `stopped`
   event **twice** (see below).

Discover names without reading plugin source by connecting to the socket
(`socat - UNIX-CONNECT:<socket>`) and sending `{"query":"launch-configs"}` or
`{"query":"adapters"}`.

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
- `src/workspace/ControlSpec.{h,cpp}` — parse the cold-start spec (incl. the
  `settings` key) and translate it to command lines (`QuoteCommandArg` keeps
  arguments `ParseCommandLine`-safe).
- `src/workspace/WorkspaceShellLsp.cpp` — thin shell forwarders, plus
  `ApplyControlSpec` (applies transient settings + auto-enable + emits `applied`
  lines), `ForceStartControlChannel`, and `ApplyStartupSettingOverrides`. The
  stdout sink (`emit_jsonl`) and `adapter_types` are wired in
  `WorkspaceShellPlugins.cpp`.

## Protocol

Newline-delimited JSON, one object per line.

| Direction | Shape |
|-----------|-------|
| request (command) | `{"id":1,"command":"breakpoint-set src/main.cpp 42"}` |
| request (query)   | `{"id":2,"query":"debug-state"}` |
| response (command)| `{"id":1,"ok":true,"feedback":"..."}` / `{"id":1,"ok":false,"error":"..."}` |
| response (query)  | `{"id":2,"ok":true,"result":{...}}` |
| event (stop began)| `{"event":"stopped","reason":"breakpoint","threadId":1,"framesPending":true}` |
| event (stop resolved)| `{"event":"stopped","file":"x.py","line":42,"reason":"breakpoint","threadId":1,"frames":[...],"framesPending":false}` |
| ready (stdout)    | `{"event":"ready","pid":..,"socket":"..","project_root":".."}` |
| applied (stdout)  | `{"applied":"breakpoint-set ..","ok":true}` |

Query verbs: `debug-state`, `breakpoints`, `tabs`, `projects`, `status`,
`launch-configs`, `adapters`.
Events: `stopped`, `terminated`, `output`. With `--control` (stdout mirror on),
events surface even with zero socket clients; responses and `applied` lines are
mirrored too. `ready`/`applied` lines are stdout-only.

**Two-phase `stopped`.** Every stop emits the `stopped` event twice, disambiguated
by `framesPending`. The first lands the instant the adapter halts, carrying the
real `reason`/`threadId` with `framesPending:true` (no `file`/`line`/`frames`) — so
an agent learns it stopped within ms even while a slow adapter (e.g. gdb indexing
DWARF for minutes) resolves the call stack. The second lands once the stack
resolves, carrying `file`/`line`/`frames` with `framesPending:false`. Only the
active session broadcasts; a background-session stop does not emit either phase.

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
