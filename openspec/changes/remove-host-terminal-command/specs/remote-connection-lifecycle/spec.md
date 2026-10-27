## REMOVED Requirements

### Requirement: Host terminals open from a command
**Reason**: A remote project's terminals already run on its host (Terminal > New); a shell on a host with no project there is `ssh` in a local terminal. The command was the sole owner of a terminal-only host session in `RemoteHostService` that duplicated the project session's state machine.
**Migration**: `Remote: Open Folder on Host…` on any directory gives a host shell with a project around it; `ssh` in a local terminal gives one without — Terminal > New in a local project, or `Remote: Open Local Terminal` (below) when only remote projects are open. The server's terminal-only hello stays accepted.

## MODIFIED Requirements

### Requirement: The host entry points sit beside their local twins
`Remote: Open Folder on Host…` SHALL appear in the File menu directly under Open Folder…, and the cold-start welcome page SHALL offer Open Folder on Host… as a second start button under Open Folder…, in its own layout slot. The command palette entry remains. A local project's terminals stay local; a remote project's terminals run on its host; and in a remote project `Remote: Open Local Terminal` (Terminal > New Local Terminal) SHALL open a terminal on this machine, the action being unavailable in a local project. A terminal tab's title SHALL say where it runs: a host terminal's title names the host, a local terminal in a remote project is prefixed `local · `, and the interactive-auth tab is prefixed `ssh · `.

#### Scenario: A user who has not read the README
- **WHEN** the editor starts with no project
- **THEN** the welcome page and the File menu both show how to open a folder on a host

#### Scenario: The welcome buttons never collide
- **WHEN** the welcome layout is computed for the no-project page at any size
- **THEN** the host-folder button lies below the open-folder button and above the recent rows, and clicking it opens the host prompt

#### Scenario: Host and local terminals are never confused
- **WHEN** a local project and a remote project are both open, each with a terminal, and the remote project also has a local terminal
- **THEN** the remote project's host terminal tab names its host and runs there, its local terminal is prefixed `local · ` and runs here, and the local project's terminal carries neither and runs here

#### Scenario: A local shell from a remote-only window
- **WHEN** the only open project is remote and the user runs `Remote: Open Local Terminal`
- **THEN** a terminal tab opens whose shell runs on this machine
