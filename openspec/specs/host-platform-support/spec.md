# host-platform-support Specification

## Purpose
Define MicroIDE's supported-host contract for Linux. Host-specific behavior must stay behind
explicit platform services for directories, processes, terminals, watchers, trash/recycle-bin
operations, packaging, launch, and host integrations.

## Requirements
### Requirement: Supported Hosts

MicroIDE SHALL support Linux as its first-class desktop host for the built-in editor, compare, merge, search, git, plugin, and terminal workflows. macOS and Windows are NOT supported hosts and have no supported build, packaging, or launch path.

#### Scenario: Linux remains a supported baseline
- **WHEN** host-platform work refactors process, watcher, terminal, file-operation, or packaging seams
- **THEN** Linux SHALL remain a supported host and SHALL continue to launch and run the built-in workflows without regression

#### Scenario: Non-Linux hosts are unsupported
- **WHEN** a contributor evaluates building or shipping MicroIDE for macOS or Windows
- **THEN** the project SHALL treat those hosts as unsupported and SHALL NOT carry current build instructions, packaging scripts, or supported-host claims for them

### Requirement: Host Services Are Explicit

Host-facing behavior SHALL be routed through explicit platform services for app directories, process launch, terminal session lifecycle, file watching, recycle-bin or trash behavior, and host integration actions such as opening URLs or revealing paths.

#### Scenario: Workspace code requests a host action
- **WHEN** workspace, project, plugin, or terminal code needs a host-facing action
- **THEN** it SHALL call a host-owned platform service rather than embedding platform-specific system calls in UI orchestration code

#### Scenario: Platform behavior stays behind the service boundary
- **WHEN** process launch, terminal, watcher, or file-manager integration needs host-specific APIs
- **THEN** the difference SHALL be isolated behind the platform service boundary while editor, compare, merge, search, and terminal rendering logic remain host-agnostic

### Requirement: Packaging And Launch Are Part Of Host Support

A host SHALL NOT be considered supported unless MicroIDE has a documented packaging and launch path for that host, including runtime asset discovery and host-correct config, cache, state, and data directories.

#### Scenario: Linux desktop launch
- **WHEN** MicroIDE is launched on Linux from a desktop build or installed package
- **THEN** it SHALL find its runtime assets, SHALL resolve app-owned directories to XDG locations, and SHALL open projects and host integrations correctly

#### Scenario: Linux packaging
- **WHEN** MicroIDE is packaged for Linux
- **THEN** the repository SHALL provide a documented local build and Debian-package path that installs the binary, runtime assets, and desktop-launcher metadata to standard locations

### Requirement: Platform Validation Is Continuous

Supported-host claims SHALL be backed by targeted automated validation and documented local bring-up instructions for Linux.

#### Scenario: New platform backend lands
- **WHEN** a change adds or modifies a host-specific backend for directories, process launch, terminal, file watching, file operations, or packaging
- **THEN** the change SHALL include targeted tests or smoke validation for Linux plus updated bring-up documentation
