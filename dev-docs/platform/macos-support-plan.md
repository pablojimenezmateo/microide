# macOS Support Plan

Reviewed on 2026-05-23.

This document tracks **remaining macOS host work** and points to the shipped baseline. It is a
host-platform plan, not a release promise.

**Authoritative contract:** [`openspec/specs/host-platform-support/spec.md`](../../openspec/specs/host-platform-support/spec.md)

**Bring-up and validation commands:** [`host-platform-bringup.md`](host-platform-bringup.md)

**Seam status table:** [`host-platform-audit.md`](host-platform-audit.md)

## Bottom Line

microide does not need a rendering rewrite for macOS. SDL3 shell, editor, compare, merge, search,
git, plugins, and terminal rendering are host-agnostic.

Most cross-platform **service seams** are already landed (directories, trash, host integration,
runtime assets, process/terminal backend splits, native watchers). What remains is mainly **Darwin
backend quality**, **real-hardware validation**, and **distribution** (signing / notarization).

## Shipped Baseline (2026-05)

Aligned with `host-platform-audit.md` and local bring-up docs:

| Area | Status |
| --- | --- |
| macOS app directories (`~/Library/Application Support`, `~/Library/Caches`) | Landed — `src/platform/AppDirectories.cpp` |
| `.app` bundle build + `Contents/Resources/assets` | Landed — see `host-platform-bringup.md` |
| Runtime asset discovery from bundle | Landed — `platform/RuntimePaths.*` |
| Trash, open URL, reveal path | Landed — `platform/Trash.*`, `platform/HostIntegration.*` |
| Process launch seam (sync + async) | Landed — `platform/ProcessBackend.*` (POSIX-backed on Darwin today) |
| Terminal PTY seam | Landed — `platform/TerminalBackend.*` (POSIX-backed on Darwin today) |
| File watching | Landed — macOS `kqueue` backend in `platform/FileWatcher.cpp` (Linux uses `inotify`) |
| Open-folder dialog | SDL `SDL_ShowOpenFolderDialog` (first pass; native picker optional later) |

Day-to-day engineering validation still happens primarily on **Linux**; macOS and Windows builds
are supported hosts per OpenSpec, not secondary experiments.

## Remaining Gaps

### 1. Darwin-native process and terminal backends (quality, not just compile)

Process and terminal code route through explicit backends, but the Darwin implementations still
share the POSIX path. Validate and harden:

- PTY launch, resize, alternate-screen apps, and process-group shutdown from **Finder / `.app`**
  launches (no terminal-inherited environment).
- Consistent environment policy for git, formatters, LSP, and plugin tool discovery when `PATH` is
  minimal.

**Exit:** Terminal and subprocess tests pass on macOS hardware; documented env policy in bring-up
notes.

### 2. macOS hardware validation matrix

CI does not replace on-machine checks for:

- Apple Silicon and Intel hosts (at least one each before calling macOS “validated”)
- Plugin load + file watch under large trees
- Compare / merge / git workflows on a real repo

**Exit:** Repeatable checklist in `host-platform-bringup.md` executed on macOS with results
recorded in the change that claims macOS validation.

### 3. Packaging, signing, and notarization

Local debug `.app` bundles build today. Distribution-quality macOS support still needs:

- codesigning and notarization steps in the release path
- clean-machine install verification
- documented troubleshooting for Gatekeeper / quarantine

**Exit:** Signed build launches on a clean macOS host without ad-hoc workarounds.

### 4. Native credential storage (follow-up)

Secret storage is not yet tied to Keychain. Not a blocker for editor/git/diff workflows, but
relevant if future features store tokens.

## Recommended Order

1. **Validate on macOS hardware** using `host-platform-bringup.md` (build, launch, focused
   `microide_tests` slice).
2. **Terminal + subprocess** — exercise from `.app` launch; fix env/PTY edge cases behind
   `TerminalBackend` / `ProcessBackend` only.
3. **Signing / notarization** — once behavior is stable on real hosts.
4. **Keychain** — only when a feature requires persisted secrets.

## Out of Scope Here

- Editor / compare / merge rendering changes
- Plugin marketplace or VS Code compatibility (see
  [`dev-docs/archive/vscode-extension-compatibility-plan.md`](../archive/vscode-extension-compatibility-plan.md))
- Claiming macOS is the primary CI host

## Related Docs

- [`host-platform-bringup.md`](host-platform-bringup.md) — build, launch, focused tests
- [`host-platform-audit.md`](host-platform-audit.md) — seam map
- [`linux-build.md`](linux-build.md) / [`windows-build.md`](windows-build.md) — other hosts
- [`../project/active-work.md`](../project/active-work.md) — current priorities
