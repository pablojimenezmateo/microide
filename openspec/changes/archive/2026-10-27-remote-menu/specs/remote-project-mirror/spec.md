## MODIFIED Requirements

### Requirement: A mirror can be removed from the editor
`Remote: Remove Mirror…` (`remote-remove-mirror <root> [discard]`) SHALL delete a closed remote project's mirror — its tree, objects, journal and state — and its recents entry, and SHALL NOT remove a mirror while a push is journaled and unsent unless `discard` is given, naming the count when it refuses. An open project's mirror SHALL be refused with "close the project first": closing the project is what runs the dirty prompt for its tabs. Run bare from the Remote menu or the palette, the command SHALL offer a pick of the closed remote projects whose mirror is on disk, each named `host:/path`; from the command line or the control channel a bare command SHALL reject with its usage. The menu row SHALL be dimmed while the recents list names no remote project that is not open.

#### Scenario: Removing a closed project's mirror from the menu
- **WHEN** a remote project has been closed and the user runs Remote > Remove Mirror…
- **THEN** a pick lists that project as `host:/path`, and choosing it deletes the mirror directory and drops the project from the recents list

#### Scenario: Removing the active project's mirror
- **WHEN** `remote-remove-mirror <root>` names the mirror of a project that is still open, with one dirty tab
- **THEN** it is refused with "close the project first" and nothing is deleted; closing the project runs the dirty prompt, after which Remove Mirror… offers it

#### Scenario: Unsent pushes
- **WHEN** the closed project has a journaled save the host has not received
- **THEN** the command refuses, naming the count, and removes the mirror only when run again with `discard`
