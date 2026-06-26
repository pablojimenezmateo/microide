# Plugin Trust Model

Reviewed on 2026-05-21. Capability sandboxing added 2026-06-15. Worker-thread
execution model and the plugin rendering surface (decorations, content surfaces,
ghost text, async edits/queries) re-reviewed 2026-06-26.

This document describes what plugins can do, what they cannot do, and what microide does and does
not enforce. It is the authoritative reference for trust-related claims about the plugin runtime.

The short version: **plugins run in-process but under an enforced per-plugin capability sandbox.**
Filesystem access through the host API is contained to the active project (and an optional
per-plugin data directory); process execution is default-deny and must be declared. On Linux,
plugin-spawned processes are additionally confined with Landlock (writes limited to the project +
data dir) and an optional seccomp network block. This narrows — but does not eliminate — the trust
you place in a plugin: a declared-process plugin can still run tools that read your project and the
system. Only install plugins you trust into your user config directory. Opening a project does not
auto-load plugin code shipped inside that repository.

Capabilities are declared in the plugin's `init.lua` descriptor table:

```lua
return ide.plugin({
  id = "my-plugin",
  capabilities = {
    fs = { read = "project", write = "project" },  -- "none" | "project" | "data"
    process = { exec = true, allow = { "eslint", "prettier" } },
    network = false,
  },
  setup = function(ctx) ... end,
})
```

Defaults when `capabilities` (or a sub-field) is omitted: `fs.read`/`fs.write` = `project`,
`process.exec` = `false`, `network` = `false`. `"data"` grants the project tree **and** the
plugin's writable data directory (`ctx.workspace.data_dir()`). An empty `process.allow` with
`exec = true` permits any binary; a non-empty list restricts `argv[0]` by basename or absolute path.

For the broader architecture see `dev-docs/plugins/plugin-runtime-research.md`. For the README summary see the
"Security & Trust Model" section of `README.md`. This file should win on disagreements.

## Where plugins come from

When microide loads, it scans one directory:

- `~/.config/microide/plugins/<plugin-id>/init.lua` — **user-scope plugins**, loaded for every
  project on the machine

Project-local plugin directories such as `<project-root>/.microide/plugins/` are **not** scanned.
That keeps cloned or untrusted repositories from executing plugin code just because you opened them.

Plugins are loaded automatically from the user config directory. There is no first-run
confirmation, no diff against a previous version, and no per-plugin disable in the UI today;
disabling a plugin means removing the file or editing user config.

`syntax/*.lua` files inside plugin directories are also loaded into the host tokenizer at startup
and on `plugins-reload`. These are interpreted as syntax rules, not as host code, but they still
come from the same trusted plugin directory and should be treated accordingly.

## Lua runtime configuration

The embedded Lua 5.4 state is created in `src/plugin/LuaRuntime.cpp`. The exposed standard library
set is intentionally narrow:

- `base` — `print`, `type`, `tostring`, `error`, `pcall`, `xpcall`, `select`, `ipairs`, `pairs`,
  `next`, `setmetatable`, `getmetatable`, `rawequal`, `rawget`, `rawset`, `rawlen`, `assert`,
  `collectgarbage`
- `table`
- `string`
- `math`
- `utf8`
- `package` — `require`, `package.path`, `package.cpath`

Not exposed:

- `io` — no `io.open`, `io.popen`, `io.lines`, `io.read`, `io.write`
- `os` — no `os.execute`, `os.getenv`, `os.exit`, `os.remove`, `os.rename`, `os.tmpname`,
  `os.time`, `os.date`

This means a plugin **cannot read or write arbitrary files using plain Lua**. It also cannot
shell out using plain Lua.

`package` is enabled, which means `require` works for pure-Lua modules and Lua-module path
resolution is available through `package.path` / `package.cpath`. There is no allowlist on those
paths.

### Execution thread and the per-call watchdog

Every `lua_State` touch runs on a dedicated **plugin worker thread**, not on the UI thread (see
`src/plugin/PluginThread.h`). The UI thread posts jobs (closures) to the worker and drains a mailbox
of plain-data results once per frame; no `lua_State*` or Lua registry ref ever crosses the thread
boundary — the worker extracts every value into native types before posting back. Execution is still
**serialized per state** (one worker, no concurrency, so Lua's single-thread-per-state rule holds),
but a slow or hung plugin call can no longer freeze the UI. Read verbs such as
`ctx.workspace.active_buffer()` and `ctx.settings.get()` resolve against a UI-owned immutable
snapshot captured at job-dispatch time, so plugin code never reads live shell state off-thread.

Each protected call is bounded by a watchdog (`LuaRuntime::PCall`): a count hook fires every 100k
Lua instructions and aborts the call once it exceeds a 750ms budget, turning an accidental infinite
loop into a contained Lua error instead of a permanent hang. This is a generous hang guard, not a
stutter guard. The one place a callback runs nested inside another call — the `ctx.process.run_async`
completion callback, which fires after a deliberately-blocking subprocess — gets its own fresh
deadline (`LuaRuntime::PCallNested`) so a legitimately long subprocess does not cause the watchdog to
abort the callback on its first instruction.

## Host API surface

The host API is what plugins actually use to do interesting things. It does grant the capabilities
that plain Lua does not. The relevant surface, by capability:

### Filesystem (capability-contained)

- `ctx.files.read_text(path)` — read a file, resolved relative to the project root
- `ctx.files.write_text(path, contents)` — write a file, project-relative
- `ctx.files.exists(path)` — existence check, project-relative
- `ctx.workspace.data_dir()` — the plugin's writable scratch directory (data-scope only)

Paths are resolved against the active project root, then **contained**: the resolved path must
stay inside the roots the plugin's `fs` capability permits (the project root, plus the data dir for
`"data"` scope). Absolute paths outside those roots, and `..` traversal that escapes them, are
refused — `read_text`/`exists` return `nil`/`false` and `write_text` returns `false`, and a denial
diagnostic naming the missing capability is written to the plugin's output channel. Containment
runs a cheap lexical check first, then `weakly_canonical` to defeat symlink-escape on paths that
exist. `fs.read`/`fs.write` may also be set to `"none"` to deny file access entirely. Within the
permitted roots there is still no per-file denylist: `.microide/`, `.git/`, and dotfiles under the
project are readable/writable.

### Process execution

- `ctx.process.run(argv, { cwd = ..., stdin = ..., env = ... })` — synchronous subprocess
  execution. Returns stdout, stderr, and exit code. The plugin chooses the argv, working
  directory, stdin payload, and environment overrides.
- `ctx.process.run_async(argv, opts, callback)` — runs the subprocess on the plugin worker thread
  (so it never blocks the UI) and invokes the callback there with the result. "Async" means
  off-the-UI-thread, not detached: the worker blocks on the child and dispatches the callback inline
  when it exits. (The earlier event-loop-wake async-process subsystem was removed.)
- Contributed language servers cause the host to launch the argv they declare on demand.
- Contributed formatters (`ctx.formatters.add(...)`) and tasks (`ctx.tasks.add(...)`) likewise
  declare an argv that the host runs.

Process execution is **default-deny**. A plugin must declare `capabilities.process.exec = true`
before `ctx.process.run` / `run_async` will spawn anything, and may further restrict `argv[0]` with
`capabilities.process.allow`. The same gate applies to spawnable contributions (language servers,
formatters, tasks): a plugin that declares one without `process.exec` is rejected at load with a
clear error. A refused `process.run` raises a Lua error and records a diagnostic naming the missing
capability. The process `cwd` is also contained to the project/data roots.

On Linux, permitted `ctx.process.run` / `run_async` children **and** contributed language-server
processes are confined in-kernel: Landlock limits writes to the project root and the plugin data dir
(the wider system stays readable/executable so tools still run), and when `network = false` a
seccomp filter blocks IPv4/IPv6 sockets (AF_UNIX/local IPC still works). This layer is best-effort
defense-in-depth: on a kernel without Landlock/seccomp it degrades to the in-process gate above. A
plugin that declares `process.exec` and runs `{"sh", "-c", "..."}` still gets a shell — but one
whose writes are confined to the project and whose network may be blocked. (Note: a language server
that needs to read or write outside the project — e.g. a package cache under `~/.nuget` or
`~/.cache` — may be restricted; the system directories and `/tmp` remain available.)

### Editor and project state

- `ctx.workspace.project_root()`, `ctx.workspace.open_file(path, line, column)`
- `ctx.active_buffer()` — path, line, column of the active buffer
- `ctx.diagnostics.publish(path, diagnostics)`, `ctx.diagnostics.clear(path_or_nil)` — publish
  diagnostics tied to the plugin's namespace
- `ctx.hover.add({ id, provide })` — register a hover provider that is queried on every editor
  hover request
- `ctx.commands.add(name, fn)` — register a plugin command callable from the command prompt
- `ctx.sidebar.add({...})` — host-rendered sidebar provider
- `ctx.completion.add(...)`, `ctx.code_actions.add(...)`, `ctx.save_participants.add(...)`,
  `ctx.tests.add(...)`, `ctx.scm.add(...)`, `ctx.annotations.add(...)`,
  `ctx.keybindings.add(...)`, `ctx.settings.declare(...)`, `ctx.menus.add(...)`,
  `ctx.status.add(...)` / `ctx.status.update(...)`
- Language contract contributions: `ctx.brackets.add(...)`, `ctx.comments.add(...)`,
  `ctx.indents.add(...)`, `ctx.snippets.add(...)`
- Buffer mutation: `ctx.editor.apply_edits({ path, edits, cursor, selection })` — the host clamps,
  validates, and applies the edits atomically (one undo step). Async edits carry a staleness guard
  (captured content revision + path) and are dropped if the buffer advanced during the worker hop,
  so a plugin cannot clobber text the user typed after the edit was computed.
- Inline suggestions: `ctx.editor.set_ghost_text(...)` / `clear_ghost_text()` — single-owner,
  caret-anchored dimmed proposal the host validates against the live caret and inserts on Tab.

Save participants are particularly worth calling out: they run on every save and can rewrite the
buffer before it hits disk. A malicious save participant could silently mutate code on every
save.

### Rendering contributions (host-renders-data)

Plugins can contribute visuals without ever touching the renderer: `ctx.decorations.set(...)`
(text styles, gutter marks, inline/virtual text, code lenses), `ctx.surface.set(...)` (a content
surface), and the ghost text above. The boundary is strict — **plugins emit validated data only;
the host owns all drawing, caching, clipping, and hit-testing.** A content surface is either a flat
**display list** (rect/line/polyline/text/clip ops) or a **raster** (PNG/JPEG, or raw RGBA8). Three
things bound this new surface as a matter of trust:

- **Resource caps, rejected not clamped.** Display lists are validated once at publish against hard
  caps (65,536 ops, 262,144 polyline points, 1 MiB text, 256 images) and must have balanced
  clip push/pop; oversize or malformed input is refused. Decoded surface pixels live in a host
  `SurfaceTextureCache` under a 256 MiB VRAM budget with LRU eviction and content-hash dedup, so a
  plugin cannot exhaust GPU memory by publishing many surfaces.
- **In-process raster decode is a new memory-safety surface.** Plugin-supplied image bytes are
  decoded off the UI thread with the vendored `stb_image` (`src/render/RasterDecode.cpp`). Encoded
  input is capped at 64 MiB and `stb_image` is compiled with `STBI_MAX_DIMENSIONS = 8192` so a
  forged header cannot trigger a multi-gigabyte `w*h*4` allocation, with a post-decode dimension
  check as defense in depth. This decoder still runs in the editor process, so — like the rest of
  the host bindings — a memory-safety bug in it is **not** contained by the capability model (see
  "What microide does not do" below); the caps and the `SurfaceRasterDecodeFuzz` /
  `PluginDisplayListParseFuzz` fuzz targets are the mitigation.
- **Hit regions reuse the validated command path.** Surface hit regions and code lenses dispatch a
  named command through the host's existing command runner — the same lookup (`HasCommand` /
  built-in action table) a command-prompt invocation uses. A plugin cannot fabricate a privileged
  built-in command it did not register; it can only invoke commands the host already exposes.

### Output and logging

- `ctx.log(message)` — append to the plugin's output channel; visible in the bottom panel

## What plugins cannot do (today)

These are real boundaries, not aspirational ones:

- **They cannot replace the editor, compare, merge, search, git, or terminal renderers.** The
  product-vision spec asserts these stay host-owned and the host ignores any plugin that declares
  itself a replacement for them. They may contribute *data* the host renders (decorations, content
  surfaces, ghost text), but never raw shell or renderer internals. This is a correctness boundary,
  not a security boundary; a hostile plugin can still publish diagnostics that fake compile errors,
  draw misleading decorations/surfaces in the editor area, or run subprocesses that modify your
  repo.
- **They cannot directly access `WorkspaceShell`.** This is policy + lint-enforced. They go
  through narrow registries.
- **They cannot read or write outside their declared filesystem scope.** Host-API file access is
  contained to the project root (and the data dir for data scope); escapes are refused.
- **They cannot run subprocesses unless they declare `process.exec`** (optionally allowlisted), and
  on Linux those subprocesses cannot write outside the project/data roots.
- **The Lua state is per-plugin, not shared.** Each loaded plugin gets its own `lua_State`, so
  plain Lua globals do not leak between plugins. `package.path` is pinned to the plugin's own
  directory and `package.cpath` / `loadlib` are disabled, so a plugin cannot `require` arbitrary
  modules or load native libraries. Plugins run in-process on a single dedicated worker thread and
  execute serially per state; they cannot spawn their own threads. `ctx.process.run_async` runs its
  subprocess on that same worker, not on a plugin-controlled thread.

## What microide does not do

These are absent today. None of them are on the roadmap unless a separate phase is opened.

- **No code signing or signature verification.** Plugins are plain files on disk.
- **No allowlist of permitted plugins.** User-scope plugins load automatically from the config
  directory.
- **No capability prompt.** Capabilities are declared in the manifest and enforced, but the host
  does not interactively ask the user to approve them on first run.
- **No isolation of the Lua state itself.** Capability enforcement happens at the host-API
  boundary and (for subprocesses) in the kernel; the `lua_State` still runs in the editor process,
  so a memory-safety bug in the host bindings is not contained by this model.
- **No kernel confinement off Linux.** macOS/Windows get the in-process capability gate (fs
  containment, process gate, allowlist) plus portable `setrlimit`, but not Landlock/seccomp. On
  Linux, both `ctx.process.run` children and contributed language-server processes are confined.
- **No marketplace, no remote install, no auto-update.** Plugins are placed by the user in
  `~/.config/microide/plugins/`.

## Recommendations

For users:

- The capability sandbox reduces, but does not eliminate, plugin trust. A plugin with
  `process.exec` can still run tools that read your whole project and the system; treat such
  plugins as you would the tools they invoke.
- Only copy or symlink plugins into `~/.config/microide/plugins/` when you trust their source.
- Opening a repository no longer executes plugin code from `.microide/plugins/` inside that repo.
- The `plugins-reload` command picks up plugin file changes between sessions. `--disable-plugins`
  / `--safe-mode` turn plugins off entirely.

Explicit scope decision:

- **Implemented (2026-06-15):** per-plugin capability declarations enforced at the fs/process
  chokepoints (default-deny process/network, project-scoped fs), path containment, a process
  allowlist, registration-time gating of spawnable contributions, restricted `package.path` /
  `package.cpath`, and Linux Landlock + seccomp + `setrlimit` confinement of plugin-spawned
  children.
- **Still out of scope:** first-run capability prompts, code signing / signed-manifest trust,
  marketplace trust, and full out-of-process isolation of the Lua state itself.
- **Git Workstation** startup trust controls remain available:
  - `--disable-plugins` — skip user-scope plugins and plugin syntax loading
  - `--safe-mode` — implies plugin disabling, skips workspace/session restore, empty shell unless
    a project path is passed; visible in status bar and Help/About
- For fully untrusted repositories, still prefer external isolation (VM/container) in addition to
  the capability sandbox and safe-mode when appropriate.

See [SECURITY.md](../SECURITY.md) and [git-workstation.md](../dev-docs/project/git-workstation.md).

For plugin authors:

- Do not silently mutate files outside the project root.
- Save participants should be deterministic and idempotent.
- Subprocess argv should be plain arrays, not shell strings (`{"git", "status"}` not
  `{"sh", "-c", "git status"}`), so it is reviewable.
- Plugins that ship example sources in a repository should disclose what they do in their
  `init.lua` header and instruct users to install them under `~/.config/microide/plugins/`.

## Out-of-scope additions

The following would be reasonable to add only if microide ever pursues plugin distribution beyond
"hand-placed local files." They are not planned now:

- first-run capability prompts that ask the user to approve a declared capability set
- a signed-manifest format and signature verification
- per-server capability overrides (e.g. granting a language server broader filesystem access than
  the default project/data confinement when it legitimately needs a home-directory cache)
- isolating each plugin in its own Lua state in a separate sandboxed process with explicit
  message-passing seams

If you want to track any of these, open an OpenSpec change rather than adding partial measures.
