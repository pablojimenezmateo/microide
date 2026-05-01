## Why

MicroIDE is still effectively Linux-first in several host-facing subsystems, which blocks Windows and macOS from being real product targets even though most of the SDL3 shell, editor, compare, merge, and AI surfaces are already portable. This needs to be corrected now because platform assumptions in process launch, terminal, file watching, directories, and packaging will otherwise harden into the wrong architecture.

## What Changes

- Add a durable host-platform support capability that defines what it means for MicroIDE to support Linux, macOS, and Windows as first-class hosts.
- Replace Linux-only or generic POSIX assumptions with explicit host-owned service seams for app directories, process launch, terminal backends, file watching, recycle-bin or trash integration, and host actions such as opening URLs or revealing paths.
- Define platform packaging and launch requirements so macOS app bundles and Windows desktop builds are part of the product contract, not ad hoc follow-up work.
- Require targeted platform validation and CI coverage for host-facing workflows instead of treating non-Linux hosts as best-effort builds.

## Capabilities

### New Capabilities
- `host-platform-support`: durable requirements for Linux, macOS, and Windows host services, packaging, and validation.

### Modified Capabilities
- None.

## Impact

- Affected code will span `src/platform/*`, `src/terminal/*`, `src/project/*`, `src/app/*`, and the CMake or packaging layer.
- Affected docs include `docs/implementation-guide.md`, `docs/active-work.md`, and the existing `docs/macos-support-plan.md`, which should align to one cross-platform host direction instead of a Linux-first baseline plus side plans.
- Affected tests and validation include host-specific filesystem, process, terminal, and launch coverage plus CI setup for macOS and Windows.
