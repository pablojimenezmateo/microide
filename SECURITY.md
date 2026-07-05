# Security

MicroIDE is a local desktop application that runs with your user privileges. Plugins run in-process
but under an enforced per-plugin capability sandbox: filesystem access is contained to the project,
process execution is default-deny, and on Linux plugin-spawned children are confined with Landlock
and seccomp. This narrows plugin trust but does not fully isolate the Lua state.

## Trust model (summary)

- **User-scope plugins only.** Lua plugins load from `~/.config/microide/plugins/<plugin-id>/`
  (or the platform-equivalent config directory). Directories such as
  `<project-root>/.microide/plugins/` in a cloned repository are **ignored** so opening a repo does
  not execute plugin code from that tree.
- **Capability-sandboxed plugins.** Plugins declare a `capabilities` table that the host enforces:
  filesystem access is contained to the project root (and an optional per-plugin data dir),
  process execution is default-deny with an optional `argv[0]` allowlist, and spawnable
  contributions (formatters / language servers / tasks) are rejected at load without `process.exec`.
  On Linux, permitted `ctx.process.run` children **and** contributed language-server processes are
  confined with Landlock (writes limited to the project + data dir) and an optional seccomp
  IPv4/IPv6 socket block.
- **Not full isolation.** There is no first-run capability prompt, no signature/marketplace trust,
  and the Lua state still runs in-process. A plugin granted `process.exec` can still run tools that
  read your project.
- **No plugin marketplace or remote install** in current scope.

See [guidelines/plugin-trust-model.md](guidelines/plugin-trust-model.md) for the full model.

## Control channel

The optional external control channel (`microide --control`, for headless / LLM-driven control)
listens on a **local `AF_UNIX` socket only** — there is no TCP listener and nothing binds to the
network. The socket is created with `0600` permissions (owner read/write only) under
`$XDG_RUNTIME_DIR/microide/`, so the trust boundary is standard Unix filesystem permissions: only
your user can connect. There is **no per-message authentication token** — this is intentional for a
single-user desktop tool and is the same model as the socket's file permissions.

When `$XDG_RUNTIME_DIR` is unset, the socket falls back to `/tmp/microide/` (a world-writable
parent directory). The socket itself is still `0600`, but on a shared multi-user host you should
prefer running with `$XDG_RUNTIME_DIR` set (the default on modern Linux desktops). Untrusted input
on the channel is bounded defensively — control descriptor files are capped (1 MiB) and the number
of concurrent control instances is limited — so a hostile local process cannot exhaust memory or
stall the CLI through the channel. See
[dev-docs/control/control-channel.md](dev-docs/control/control-channel.md) for the full protocol.

## Safe startup flags (Git Workstation)

For recovery or inspection of unfamiliar repositories:

| Flag | Behavior |
|------|----------|
| `--disable-plugins` | Skips user-scope Lua plugins and plugin syntax loading. Built-in editor, Git, diff, merge, search, and terminal workflows remain available. |
| `--safe-mode` | Implies `--disable-plugins`, skips workspace/session restore at startup, opens only an explicit project path argument or an empty shell, and shows **Safe mode** in the status bar and Help/About. |

Example:

```bash
microide --safe-mode /path/to/repo
microide --disable-plugins
```

These flags disable plugins entirely; they are distinct from the per-plugin capability sandbox,
which applies whenever plugins are enabled. Neither isolates built-in features or code you run
outside MicroIDE.

## Reporting security issues

If you believe you have found a security vulnerability, please report it privately to the
maintainers (project contact on the repository hosting page). Include:

- MicroIDE version or commit
- Operating system
- Steps to reproduce
- Impact you observed (data exposure, arbitrary code execution path, etc.)

Do not open public issues for undisclosed vulnerabilities until maintainers acknowledge receipt.

## Reporting crashes and data loss

For the Git Workstation, also report:

- Whether `--safe-mode` or `--disable-plugins` was used
- Project path (redact secrets)
- Whether unsaved editor buffers or merge results were lost
- Persisted state paths under your config directory if relevant (`workspace-session`, project
  session files)

Use the repository issue tracker with the **bug** label. Attach logs if `MICROIDE_STARTUP_TRACE`
or `MICROIDE_PERF_TRACE` were enabled.

## Known limitations

- Per-plugin capability sandbox enforced (fs containment, default-deny process, Linux
  Landlock/seccomp confinement of both `ctx.process.run` children and contributed language servers);
  but no first-run capability prompt, plugin signing, marketplace trust, or out-of-process isolation
  of the Lua state
- Release binaries are GPG-signed; verify the detached signature and SHA256 checksum before
  installing (see README → Verifying releases). Plugin signing / marketplace trust is still a non-goal.
- Comparative performance claims against other editors are not made; internal baselines only
