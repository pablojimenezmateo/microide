## MODIFIED Requirements

### Requirement: Supported Hosts

MicroIDE SHALL support Linux, macOS, and Windows as first-class desktop hosts for the built-in editor, compare, merge, search, git, plugin, and terminal workflows.

#### Scenario: Linux remains a supported baseline
- **WHEN** host-platform work refactors process, watcher, terminal, file-operation, or packaging seams
- **THEN** Linux SHALL remain a supported host and SHALL continue to launch and run the built-in workflows without regression

#### Scenario: macOS is treated as a supported host
- **WHEN** MicroIDE is built for macOS
- **THEN** the application SHALL launch with the built-in workflows available, SHALL use macOS-correct host services, and SHALL NOT rely on Linux directory or watcher policies

#### Scenario: Windows is treated as a supported host
- **WHEN** MicroIDE is built for Windows
- **THEN** the application SHALL launch with the built-in workflows available, SHALL use Windows-correct host services, and SHALL NOT fail because Linux-only or POSIX-only terminal, process, watcher, or trash assumptions remain in the host path
