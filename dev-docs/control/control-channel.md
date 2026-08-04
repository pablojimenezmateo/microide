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

### Driving a live instance headlessly

To exercise a long-lived `--control` instance (rather than a one-shot spec):

```bash
xvfb-run -a env SDL_AUDIODRIVER=dummy XDG_RUNTIME_DIR=/run/user/1000 \
  ./build/microide/microide <project> --control &
XDG_RUNTIME_DIR=/run/user/1000 ./build/microide/microide control-send <verb> [args]
```

Three gotchas, each of which cost real time:

- **It must run under `xvfb`.** With `SDL_VIDEODRIVER=dummy` alone (no X display)
  the app exits right after the sandbox-init log line. xvfb keeps the window alive.
  (This is the opposite of the perf harness, which wants the dummy driver — see
  `dev-docs/performance/perf-harness.md`.)
- **`XDG_RUNTIME_DIR` must be a SHORT path.** The control socket is
  `$XDG_RUNTIME_DIR/microide/<pid>.sock`, and AF_UNIX caps the path at 108 bytes.
  A long scratch path silently fails to bind — no socket, no error. Use the real
  `/run/user/<uid>`.
- **Kill by PID, never `pkill -f 'build/microide/microide'`** — that pattern
  matches the invoking shell's own command line. Use
  `ps -eo pid,args | awk '$2 ~ /\/microide\/microide$/ {print $1}'`.

The channel exposes no buffer-content query, so verify edits by driving `save` and
reading the file from disk. Watch for cross-command state: `add-cursor-all-matches`
leaves multiple carets that pollute later single-caret edits, and a buffer right
after `open` may not have settled — settle before asserting.

Discover names without reading plugin source with the one-shot client:
`microide control-send --query launch-configs` or `microide control-send --query
adapters` (see below).

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
- `src/workspace/control/ControlChannelService.{h,cpp}` — owns the server, drains inbound
  requests on the control SDL wake event (`ConsumeControlCallbacks`), dispatches
  commands through the `Operations.execute_command_line` seam, answers queries by
  reading `WorkspaceContext`, and broadcasts debug events (wired from the
  `DebugService` operations in `WorkspaceShellPlugins.cpp`).
- `src/workspace/control/ControlProtocol.{h,cpp}` — request/response/event (de)serialize
  plus `ControlChannelHelpText()`, the single source of truth for the protocol
  text rendered by `microide control-help` and the man page.
- `src/workspace/control/ControlSpec.{h,cpp}` — parse the cold-start spec (incl. the
  `settings` key) and translate it to command lines (`QuoteCommandArg` keeps
  arguments `ParseCommandLine`-safe).
- `src/workspace/shell/WorkspaceShellLsp.cpp` — thin shell forwarders, plus
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
| event (terminated)| `{"event":"terminated","sessionId":1}` (clean) / `{"event":"terminated","sessionId":1,"reason":"debug adapter exited unexpectedly"}` (crash/kill/launch-reject) |
| ready (stdout)    | `{"event":"ready","pid":..,"socket":"..","project_root":".."}` |
| applied (stdout)  | `{"applied":"breakpoint-set ..","ok":true}` |

Query verbs: `debug-state`, `breakpoints`, `function-breakpoints`,
`exception-filters`, `tabs`, `projects`, `status`, `launch-configs`, `adapters`.
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

**`terminated` fires for every end.** A session broadcasts `terminated` on *any*
terminal transition — a clean DAP `terminated`/`exited`, a launch/attach rejection,
or the adapter process dying with no DAP event (crash / external kill / RLIMIT_AS
cap). A non-clean end carries a `reason`; a clean exit omits it. This is driven off
the `DebugSession::on_terminated` callback (fired once via the absorbing terminal
guard), so an observer is never stranded waiting on a `terminated` that a crashing
adapter never sent. The `sessionId` is the real session id.

Line numbers are 1-based on every developer-facing surface (commands, spec,
events, queries); the single 1-based→0-based conversion lives in the
breakpoint-command executor (`WorkspaceGlobalActionExecutor.cpp`).

## Breakpoint commands

`breakpoint-set`, `breakpoint-remove`, `breakpoint-enable`, `breakpoint-disable`,
`breakpoint-condition`, `breakpoint-hit-condition`, `breakpoint-logmessage`,
`breakpoint-clear`, plus session launch via `debug-launch [name]` (named config)
or `debug-run [--type <adapter>] <program> [args...]` (ad-hoc launch by program
path — no pre-defined config needed). All breakpoint commands take an explicit
`<file> <line>` (unlike the context-menu breakpoint modifiers, which read the
gutter line). Breakpoint/debug commands require `debug.enabled`, but over the
control channel any `breakpoint-`/`debug-` command **auto-enables it transiently**
(no `set-setting debug.enabled true` prelude); they re-send to a live session via
`DebugService::ResendBreakpointsForFile`. Breakpoints placed this way persist
through the normal `PersistedDebugState` path.

**Function breakpoints** break on a symbol with no file/line — the right tool when
an agent is told only a function name: `breakpoint-function-add <name>`,
`breakpoint-function-remove <name>`, `breakpoint-function-toggle <name>`,
`breakpoint-function-condition <name> [expr]`. Exception-filter conditions use
`breakpoint-exception-condition <filterId> [expr]`. Query current state with the
`function-breakpoints` and `exception-filters` verbs.

## Review / diff commands

Three verbs let an agent (or a human via the control channel) bulk-open the
diff/merge tabs needed to review changes. Each one switches to the **Source
Control** view, dedupes against already-open tabs, and closes stale *clean* review
tabs from a prior run (dirty/edited review and merge tabs are always kept). The
reply `feedback` summarizes counts and lists the opened files, e.g.
`review-conflicts: opened 3, reused 1, closed 1 — src/a.cpp, src/b.cpp, src/c.cpp`.

- `review-conflicts` — one **merge** tab per conflicted file in the working tree.
  **Non-mutating**: it never runs `git merge`. Run the merge yourself first, then
  open the conflicts:
  ```bash
  microide control-send term "git merge origin/main"
  microide control-send review-conflicts
  ```
- `review-branch [ref]` — one **compare** tab (working tree vs `ref`) per differing
  file, i.e. "what is different between local state and `ref`". `ref` is any
  commit-ish (branch, tag, hash, `HEAD~3`). Omit it (or pass `origin`) to use the
  repo base branch via `ResolveGitBaseReference` (origin/HEAD, else main/master,
  else `@{upstream}`).
- `review-commit [commit]` — one **compare** tab (`commit~1` vs `commit`) per file
  that commit changed; `commit` is any commit-ish and defaults to `HEAD` (review the
  last commit). Pass a hash to review any historical commit.

The orchestration lives in `ReviewSessionCoordinator` (host-owned tab lifecycle +
Source Control switch injected as callbacks); the pure open/reuse/close
reconciliation is `ComputeReviewTabPlan` (`src/workspace/git/ReviewTabPlan.h`). File
enumeration reuses `project::CollectGitWorkingTreeEntries` (conflicts),
`CollectGitWorkingTreeDiffFiles` (branch), and `CollectGitCommitChangedFiles`
(commit); tab opening reuses `CompareMergeService` (which dedupes internally).

## One-shot client (`microide control-send`)

`microide control-send` is the reliable way to drive a running instance from a
script or an agent — no socket plumbing, no JSON hand-crafting. It connects to the
target instance (auto-selecting the sole running one, else `--pid`/`--socket`),
sends one request, keeps the connection open until the reply (and, with `--wait`,
the awaited event) arrives, prints every JSONL line to stdout, and exits:

```bash
microide control-send breakpoint-function-add main      # {"command":"..."}
microide control-send --query launch-configs            # {"query":"..."}
microide control-send --json '{"command":"debug-start"}' # sent verbatim
microide control-send debug-run ./build/app --wait stopped  # block until it stops
```

Options: `--pid <n>` / `--socket <path>` (target), `--timeout <secs>` (default 5),
`--wait <event>` (block until that event; `stopped` waits for the resolved frame),
`--id <n>`. Exit codes: `0` reply ok, `1` reply not ok, `2` usage/discovery/
connect, `3` `--wait` timed out. The reliability rests on the server's graceful
half-close: a client that closes its write side still receives replies for the
requests it already sent (`ControlSocketServer` lingers until they flush), so the
old half-closed-client "no reply" footgun is gone — though `control-send` is still
the recommended primitive.

## Recipes

**Set up a session and hand the live window to the human** (the "give me control"
request). `--set control.enabled true` *without* `--control` opens a normal
interactive window with the socket live — the human keeps driving after the agent
finishes. Any `breakpoint-`/`debug-` command auto-enables the debugger, so no
`debug.enabled` prelude is needed:

```bash
microide /path/to/project --set control.enabled true &   # normal window + socket
microide control-send breakpoint-function-add A          # debugger auto-enables
microide control-send debug-run ./build/app              # or: debug-launch <config>
# leave the window open — the human now drives it interactively.
```

`debug-run [--type <adapter>] <program> [args...]` synthesizes a transient launch
config from a binary path, so no pre-defined launch config is required (the
bundled `gdb-dap` plugin supplies a `gdb` adapter for native code). Define a
project launch config of type `gdb` for a reusable target.

**Investigate a crash, break just before the suspect line** (headless): use
`--control --control-spec` with a `{file,line}` (or `functionBreakpoints`) entry,
read launch names via the `launch-configs` query first, then watch for `stopped`.

## Discovery & security

Socket: `$XDG_RUNTIME_DIR/microide/<pid>.sock` (0600). Descriptor:
`$XDG_RUNTIME_DIR/microide/instances/<pid>.json` with `{pid, socket,
project_root, project_hash}`. `microide control-list` enumerates them. The
channel grants command-palette-level power, so it is opt-in per instance and the
socket is user-private.
