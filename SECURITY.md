# Security

MicroIDE is a local desktop application. It runs with your user privileges and does not sandbox
plugins or subprocesses launched through the host API.

## Trust model (summary)

- **User-scope plugins only.** Lua plugins load from `~/.config/microide/plugins/<plugin-id>/`
  (or the platform-equivalent config directory). Directories such as
  `<project-root>/.microide/plugins/` in a cloned repository are **ignored** so opening a repo does
  not execute plugin code from that tree.
- **Plugins are trusted local code.** The host API exposes project file I/O, subprocess launch,
  LSP registration, diagnostics, sidebars, and save participants. There is no signature check,
  capability prompt, or per-plugin filesystem allowlist today.
- **No plugin marketplace or remote install** in current scope.

See [docs/plugin-trust-model.md](docs/plugin-trust-model.md) for the full model.

## Safe startup flags (Git Workstation Preview)

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

These flags are **not** a sandbox. They reduce moving parts at startup; they do not isolate network,
filesystem, or subprocess access from built-in features or from code you run outside MicroIDE.

## Reporting security issues

If you believe you have found a security vulnerability, please report it privately to the
maintainers (project contact on the repository hosting page). Include:

- MicroIDE version or commit
- Operating system
- Steps to reproduce
- Impact you observed (data exposure, arbitrary code execution path, etc.)

Do not open public issues for undisclosed vulnerabilities until maintainers acknowledge receipt.

## Reporting crashes and data loss (preview)

For the Git Workstation Preview, also report:

- Whether `--safe-mode` or `--disable-plugins` was used
- Project path (redact secrets)
- Whether unsaved editor buffers or merge results were lost
- Persisted state paths under your config directory if relevant (`workspace-session`, project
  session files)

Use the repository issue tracker with the **bug** label. Attach logs if `MICROIDE_STARTUP_TRACE`
or `MICROIDE_PERF_TRACE` were enabled.

## Known limitations

- No plugin sandbox, signing, or marketplace trust layer
- No signed release binaries in the preview track (build from source; verify checksums when published)
- Comparative performance claims against other editors are not made; internal baselines only
