## MODIFIED Requirements

### Requirement: The host entry points sit beside their local twins
The remote verbs SHALL live in a top-level `Remote` menu between Git and Terminal: Open Folder on Host…, Show Status, Show Changed on Host, Show Host Server Log, Forward a Port…, Stop Forwarding a Port…, Reconnect, Disconnect, Stop Host Server and Remove Mirror…, with their menu labels carrying no `Remote:` prefix while their command-palette labels keep it. The File menu SHALL NOT also list Open Folder on Host…. The cold-start welcome page SHALL offer Open Folder on Host… as a second start button under Open Folder…, in its own layout slot. The Terminal menu keeps New Local Terminal, available in a remote project. An item whose action is not available (no host connected) SHALL be shown dimmed rather than hidden. The status bar's remote segment SHALL open the Remote menu as a popup anchored at the segment when clicked, in place of any single command. In Compact mode the Remote menu SHALL be in the hamburger popup like every other top-level menu, and when the bar overflows it SHALL be in the chevron's popup in order, never dropped. A terminal tab's title SHALL say where it runs: a host terminal's title names the host, a local terminal in a remote project is prefixed `local · `, and the interactive-auth tab is prefixed `ssh · `.

#### Scenario: A user who has not read the README
- **WHEN** the editor starts with no project
- **THEN** the welcome page and the Remote menu both show how to open a folder on a host

#### Scenario: The welcome buttons never collide
- **WHEN** the welcome layout is computed for the no-project page at any size
- **THEN** the host-folder button lies below the open-folder button and above the recent rows, and clicking it opens the host prompt

#### Scenario: Everything remote is in one place
- **WHEN** a remote project is connected and the user opens the Remote menu
- **THEN** every remote verb that needs no typed argument is listed, labelled without the `Remote:` prefix, and the same verbs are found in the palette by typing `remote`

#### Scenario: The host's segment is the shortcut
- **WHEN** the user clicks the status bar's segment naming the connected host
- **THEN** the Remote menu opens as a popup above the segment, and Escape closes it

#### Scenario: Nothing to act on
- **WHEN** no host is connected and the user opens the Remote menu
- **THEN** Open Folder on Host… is available and the connection verbs are dimmed, not missing

#### Scenario: Host and local terminals are never confused
- **WHEN** a local project and a remote project are both open, each with a terminal, and the remote project also has a local terminal
- **THEN** the remote project's host terminal tab names its host and runs there, its local terminal is prefixed `local · ` and runs here, and the local project's terminal carries neither and runs here

#### Scenario: A local shell from a remote-only window
- **WHEN** the only open project is remote and the user runs `Remote: Open Local Terminal`
- **THEN** a terminal tab opens whose shell runs on this machine
