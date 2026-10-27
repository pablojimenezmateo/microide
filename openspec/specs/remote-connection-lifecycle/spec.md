
## Purpose

Define how the client reaches a host (one ssh ControlMaster, interactive auth in a terminal), starts or installs `microide-server`, and tracks and recovers the connection.

## Requirements

### Requirement: One ssh master per host and one channel per project, over stock sshd
The client SHALL establish one ssh ControlMaster per `(user, host, port)` with `BatchMode=yes` and reach the server through one channel per open project. It SHALL require no inbound port, no port forwarding, no `sshd_config` change and no root on the host. Host and user strings SHALL be validated against `[A-Za-z0-9._-]`, SHALL NOT start with `-`, and SHALL follow `--` in every ssh argv. The control path SHALL fit the 108-byte AF_UNIX limit.

#### Scenario: A host string that looks like an option is refused
- **WHEN** the user enters `-oProxyCommand=evil` as a host
- **THEN** it is rejected before any process is spawned

#### Scenario: A MaxSessions 1 host works
- **WHEN** the host's sshd allows one session per connection and the user opens three host terminals and runs a build
- **THEN** everything runs over the one channel with no second connection or authentication

### Requirement: Authentication happens in a terminal tab
When the batch connection fails for want of a credential or an unknown host key, the client SHALL open a terminal tab running the same ssh command without `BatchMode` and wait for the control socket to appear. No password field SHALL exist and no credential SHALL be stored.

#### Scenario: A passphrase-protected key
- **WHEN** the user's key needs a passphrase
- **THEN** a terminal tab prompts for it, and the connection proceeds once the user answers there

### Requirement: Attach-or-start, then install when missing or mismatched
The client SHALL run `remote.server_command`, else `~/.local/share/microide/server/microide-server`, else `microide-server` on `PATH`, with `attach`. If the command is not found or the handshake reports an incompatible protocol, and `remote.server_install` is not `off`, the client SHALL copy the matching static server into `~/.local/share/microide/server/` over the existing connection using the single fixed install script (directory passed as `$0`, created 0700, binary 0700, written to a temporary name then renamed) and retry. With install off it SHALL show the copy command instead. A mismatch with live terminals SHALL install beside the running server and ask before restarting it. A compatible server OLDER than the client's release SHALL keep serving (with its terminals) while the client installs its own release beside it and offers a restart; a server that is newer, or whose release cannot be ordered, SHALL be left alone, so an older client never downgrades a host.

#### Scenario: First connect to a host with no server
- **WHEN** the host has no `microide-server` anywhere
- **THEN** the client installs it, shows an Installing progress row throughout, and reaches Ready without any action from the user

### Requirement: Connection states are visible and recoverable
The client SHALL track Disconnected, Connecting, NeedsAuth, StartingServer, Installing, Ready, Reconnecting and Offline. A dead link SHALL move Ready to Reconnecting, which SHALL attach with resume. A status-bar segment SHALL show the host and state, and `Remote: Show Status` SHALL show the details. Failures SHALL be sticky notification rows with inline actions (Reconnect, Show Log, Copy Install Command, Copy ssh Command as applicable) that clear when the state does.

#### Scenario: A laptop wakes on a new network
- **WHEN** the link is declared dead and connectivity returns
- **THEN** the client reconnects, every host terminal resumes from its offset, and the Reconnecting row disappears

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
