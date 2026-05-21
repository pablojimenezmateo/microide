# Plugin Trust Model

Reviewed on 2026-05-18.

This document describes what plugins can do, what they cannot do, and what microide does and does
not enforce. It is the authoritative reference for trust-related claims about the plugin runtime.

The short version: **plugins are trusted local code that runs with the same privileges as your
microide process.** Treat opening a project as roughly equivalent to executing the code shipped in
that repository.

For the broader architecture see `docs/plugin-runtime-research.md`. For the README summary see the
"Security & Trust Model" section of `README.md`. This file should win on disagreements.

## Where plugins come from

When microide loads, it scans two directories per project:

- `~/.config/microide/plugins/<plugin-id>/init.lua` — **user-scope plugins**, loaded for every
  project on the machine
- `<project-root>/.microide/plugins/<plugin-id>/init.lua` — **project-scope plugins**, loaded when
  that specific project is opened

Both run with identical privileges. There is no separation between "trusted user plugin" and
"plugin that arrived inside a repository you just cloned." The runtime treats them the same.

Plugins are loaded automatically. There is no first-run confirmation, no diff against a previous
version, and no per-plugin disable in the UI today; disabling a plugin means removing the file or
editing the project / user config.

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

## Host API surface

The host API is what plugins actually use to do interesting things. It does grant the capabilities
that plain Lua does not. The relevant surface, by capability:

### Filesystem (project-scoped)

- `ctx.files.read_text(path)` — read a file, resolved relative to the project root
- `ctx.files.write_text(path, contents)` — write a file, project-relative
- `ctx.files.exists(path)` — existence check, project-relative

Paths are resolved against the active project root. Absolute paths are honored when passed.
There is no allowlist or denylist on which files inside the project can be read or written;
`.microide/`, `.git/`, dotfiles, and parent traversal via `..` are not specifically blocked at
this layer.

### Process execution

- `ctx.process.run(argv, { cwd = ..., stdin = ..., env = ... })` — synchronous subprocess
  execution. Returns stdout, stderr, and exit code. The plugin chooses the argv, working
  directory, stdin payload, and environment overrides.
- `ctx.process.run_async(argv, opts, callback)` — async variant; the host wakes the event loop
  when the process exits and dispatches the callback.
- Contributed language servers cause the host to launch the argv they declare on demand.
- Contributed formatters (`ctx.formatters.add(...)`) and tasks (`ctx.tasks.add(...)`) likewise
  declare an argv that the host runs.

A plugin that calls `ctx.process.run({"sh", "-c", "..."})` has full shell access. There is no
capability gate, no per-plugin allowlist of binaries, no prompt.

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

Save participants are particularly worth calling out: they run on every save and can rewrite the
buffer before it hits disk. A malicious save participant could silently mutate code on every
save.

### Output and logging

- `ctx.log(message)` — append to the plugin's output channel; visible in the bottom panel

## What plugins cannot do (today)

These are real boundaries, not aspirational ones:

- **They cannot replace the editor, compare, merge, search, git, or terminal renderers.** The
  product-vision spec asserts these stay host-owned and the host ignores any plugin that declares
  itself a replacement for them. This is a correctness boundary, not a security boundary; a
  hostile plugin can still publish diagnostics that fake compile errors or run subprocesses that
  modify your repo.
- **They cannot directly access `WorkspaceShell`.** This is policy + lint-enforced. They go
  through narrow registries.
- **The Lua state is per-plugin, not shared.** Each loaded plugin gets its own `lua_State`, so
  plain Lua globals do not leak between plugins. This is a small correctness boundary, not a
  sandbox: every plugin still runs in-process and still receives host APIs that can read files,
  launch subprocesses, and affect the active project. Plugins also still run sequentially (no
  plugin background threading except via `ctx.process.run_async`'s host-managed wake).

## What microide does not do

These are absent today. None of them are on the roadmap unless a separate phase is opened.

- **No code signing or signature verification.** Plugins are plain files on disk.
- **No allowlist of permitted plugins.** Both user and project plugins load automatically.
- **No capability prompt.** The host never asks "this plugin wants to run a subprocess, allow?"
- **No per-plugin filesystem namespacing.** A plugin reading `~/.ssh/id_rsa` through
  `ctx.files.read_text` is not blocked at the API layer (it requires an absolute path; the API
  does not refuse one).
- **No restricted-mode `require`.** `package.path` and `package.cpath` are not pinned to a
  microide-controlled directory.
- **No process isolation.** The Lua state runs in-process with the editor.
- **No marketplace, no remote install, no auto-update.** Plugins are placed by the user (or by
  whoever wrote the repo you opened).

## Recommendations

For users:

- Treat opening a microide project as equivalent to running arbitrary code from that repository.
  This is also true of editors that run `Makefile`, `package.json` lifecycle scripts, or
  `direnv`, but microide makes the plugin surface explicit.
- For untrusted repositories: inspect `.microide/plugins/` and `.microide/` config before opening,
  or open inside a VM / container.
- `plugins-reload` picks up plugin file changes between sessions; there is no
  "all-plugins-disabled" run mode in the UI today.

Explicit scope decision:

- Plugin security-system hardening is out of current product scope. This includes safe-mode
  startup paths, plugin sandboxing / process isolation, and first-run capability prompts.
- A safe-mode or project-plugin-disable startup path is not planned in the current scope.
- If you need to inspect untrusted repositories, use external isolation (VM/container) before
  opening them in microide.

For plugin authors:

- Do not silently mutate files outside the project root.
- Save participants should be deterministic and idempotent.
- Subprocess argv should be plain arrays, not shell strings (`{"git", "status"}` not
  `{"sh", "-c", "git status"}`), so it is reviewable.
- Plugins that ship in a repository should disclose what they do in their `init.lua` header.

## Out-of-scope additions

The following would be reasonable to add only if microide ever pursues plugin distribution beyond
"hand-placed local files." They are not planned now:

- per-plugin capability declarations and prompts on first run
- restricted `package.path` / `package.cpath` pinned to plugin install dir
- a signed-manifest format
- isolating each plugin in its own Lua state with explicit message-passing seams

If you want to track any of these, open an OpenSpec change rather than adding partial measures.
