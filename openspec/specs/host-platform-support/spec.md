# host-platform-support Specification

## Purpose
TBD - created by archiving change add-cross-platform-host-support. Update Purpose after archive.
## Requirements
### Requirement: Supported Hosts

MicroIDE SHALL support Linux, macOS, and Windows as first-class desktop hosts for the built-in editor, compare, merge, search, git, AI, plugin, and terminal workflows.

#### Scenario: Linux remains a supported baseline
- **WHEN** host-platform work refactors process, watcher, terminal, file-operation, or packaging seams
- **THEN** Linux SHALL remain a supported host and SHALL continue to launch and run the built-in workflows without regression

#### Scenario: macOS is treated as a supported host
- **WHEN** MicroIDE is built for macOS
- **THEN** the application SHALL launch with the built-in workflows available, SHALL use macOS-correct host services, and SHALL NOT rely on Linux directory or watcher policies

#### Scenario: Windows is treated as a supported host
- **WHEN** MicroIDE is built for Windows
- **THEN** the application SHALL launch with the built-in workflows available, SHALL use Windows-correct host services, and SHALL NOT fail because Linux-only or POSIX-only terminal, process, watcher, or trash assumptions remain in the host path

### Requirement: Host Services Are Explicit

Host-facing behavior SHALL be routed through explicit platform services for app directories, process launch, terminal session lifecycle, file watching, recycle-bin or trash behavior, and host integration actions such as opening URLs or revealing paths.

#### Scenario: Workspace code requests a host action
- **WHEN** workspace, project, plugin, or terminal code needs a host-facing action
- **THEN** it SHALL call a host-owned platform service rather than embedding platform-specific system calls or Linux-only assumptions in UI orchestration code

#### Scenario: Platform backend differs by host
- **WHEN** Linux, macOS, and Windows require different APIs for process launch, terminal, watcher, or file-manager integration
- **THEN** the difference SHALL be isolated behind the platform service boundary while editor, compare, merge, search, and terminal rendering logic remain host-agnostic

### Requirement: Packaging And Launch Are Part Of Host Support

A host SHALL NOT be considered supported unless MicroIDE has a documented packaging and launch path for that host, including runtime asset discovery and host-correct config, cache, state, and data directories.

#### Scenario: macOS desktop launch
- **WHEN** MicroIDE is launched on macOS from Finder or as an app bundle
- **THEN** it SHALL find its runtime assets, SHALL resolve app-owned directories to macOS locations, and SHALL open projects and host integrations without assuming a terminal-inherited shell environment

#### Scenario: Windows desktop launch
- **WHEN** MicroIDE is launched on Windows from its desktop build output
- **THEN** it SHALL find its runtime assets, SHALL resolve app-owned directories to Windows locations, and SHALL provide correct recycle-bin, process, and terminal-host behavior for built-in workflows

### Requirement: Platform Validation Is Continuous

Supported-host claims SHALL be backed by targeted automated validation and documented local bring-up instructions for Linux, macOS, and Windows.

#### Scenario: New platform backend lands
- **WHEN** a change adds or modifies a host-specific backend for directories, process launch, terminal, file watching, file operations, or packaging
- **THEN** the change SHALL include targeted tests or smoke validation for the affected host plus updated bring-up or CI documentation

#### Scenario: CI review for supported hosts
- **WHEN** Linux, macOS, or Windows is listed as a supported host in durable docs or specs
- **THEN** the repository SHALL include a corresponding validation path in CI or a documented temporary gap with a defined follow-up plan

