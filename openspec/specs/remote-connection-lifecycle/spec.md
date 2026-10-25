
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

### Requirement: Host terminals open from a command
`Remote: Open Terminal on Host…` SHALL prompt for a host and open a terminal tab whose title names the host (`build-box · zsh`). A local project's terminals stay local unless opened through this command (a remote PROJECT's terminals run on its host; see the remote-projects design, Phase 2b).

#### Scenario: Host and local terminals are never confused
- **WHEN** a local project has one local terminal and one host terminal open
- **THEN** their tab titles name `local` and the host respectively

### Requirement: The host entry points sit beside their local twins
`Remote: Open Folder on Host…` SHALL appear in the File menu directly under Open Folder…, `Remote: Open Terminal on Host…` in the Terminal menu directly under New Terminal, and the cold-start welcome page SHALL offer Open Folder on Host… as a second start button under Open Folder…, in its own layout slot. The command palette entries remain.

#### Scenario: A user who has not read the README
- **WHEN** the editor starts with no project
- **THEN** the welcome page and the File menu both show how to open a folder on a host

#### Scenario: The welcome buttons never collide
- **WHEN** the welcome layout is computed for the no-project page at any size
- **THEN** the host-folder button lies below the open-folder button and above the recent rows, and clicking it opens the host prompt
