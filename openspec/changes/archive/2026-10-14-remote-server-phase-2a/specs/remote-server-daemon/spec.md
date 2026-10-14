## ADDED Requirements

### Requirement: One server per user serving many roots
`microide-server` SHALL run at most one daemon per uid. It SHALL bind one AF_UNIX socket and open each root a client asks for as a workspace with its own processes, terminals and budgets. A workspace SHALL close only when no client is attached, its idle timer has expired, and it owns no live terminal or kept process.

#### Scenario: Two projects on one host share one server
- **WHEN** a client opens terminals for two different roots on the same host
- **THEN** one server process serves both, and `microide-server status` lists two workspaces

### Requirement: Subcommands start, stop, status and attach
The binary SHALL provide `start` (daemonize and stay up), `stop`, `status` (print the socket, workspaces, terminals and processes) and `attach` (connect to the running server, or start one, then relay stdio to it). A server started with `start` SHALL never exit for idleness; one started on demand by `attach` SHALL exit when it has no workspaces and its idle timer expires.

#### Scenario: A hand-started server survives a night with no clients
- **WHEN** a user runs `microide-server start`, connects, and disconnects for longer than the idle timeout
- **THEN** the server is still running and the next `attach` reaches it without a restart

#### Scenario: An on-demand server exits when idle
- **WHEN** `attach` started the server and every workspace has closed and the idle timeout passes
- **THEN** the server exits and removes its socket

### Requirement: The socket lives under the user's home and is verified on every bind
The socket directory SHALL default to `~/.local/state/microide/server/`, created mode 0700, and SHALL be verified on every bind: each component opened without following symlinks, owned by the uid, and mode 0700. A directory that fails verification SHALL be refused with the reason and never adopted. `$XDG_RUNTIME_DIR` SHALL NOT be required; `remote.server_socket_dir` SHALL override the location. The socket path SHALL fit the 108-byte AF_UNIX limit.

#### Scenario: A group-writable socket directory is refused
- **WHEN** the socket directory exists with mode 0770
- **THEN** the server refuses to bind and reports the directory, its mode and the expected mode

#### Scenario: A symlinked component is refused
- **WHEN** any component of the socket directory path is a symlink
- **THEN** the server refuses to bind and names the symlinked component

### Requirement: Daemonization detaches stdio before success is reported
A daemon started by `attach` or `start` SHALL `setsid` and reopen descriptors 0, 1 and 2 on `/dev/null` before the starter reports success, writing its diagnostics to a size-capped log file beside the socket. The starter SHALL determine success by the socket becoming connectable, never by the spawned process exiting. Every descriptor the server creates SHALL be close-on-exec at creation.

#### Scenario: The ssh command that started the server returns
- **WHEN** `ssh host -- microide-server attach` starts a new daemon and the relay connects
- **THEN** the ssh connection is not held open by the daemon's inherited descriptors

### Requirement: The host's session-survival policy is reported
At start the server SHALL read `/etc/systemd/logind.conf` and its drop-ins and report `kill_user_processes` and `linger` in `server/hello`. It SHALL NOT use SysV IPC or POSIX shared memory.

#### Scenario: A host that kills user processes at logout is named at connect
- **WHEN** the host sets `KillUserProcesses=yes` without linger for the user
- **THEN** the client shows a sticky notification saying terminals will not survive a disconnect and naming the two settings an administrator can change

### Requirement: A server instance has an epoch
Each server process SHALL have a `daemon_epoch` unique to it. Any resume token (terminal offsets, process offsets, and in Phase 2b manifest ids) from a different epoch SHALL be refused rather than interpreted, and the client SHALL fall back to a cold attach.

#### Scenario: A restarted server does not claim to remember
- **WHEN** a client reattaches with handle offsets issued by a previous server process
- **THEN** the server reports the epoch mismatch and the client reopens its terminals cold, with no duplicated or silently skipped output

### Requirement: The server links only the windowing-free kernel
`microide-server` SHALL link only `microide_kernel` objects and its own sources: no SDL, SDL_ttf or fontconfig. Release builds SHALL be statically linked against musl and SHALL resolve the user from `$HOME` and `getuid()` only, never through NSS.

#### Scenario: The server binary has no windowing dependency
- **WHEN** the release `microide-server` is inspected
- **THEN** it is a static executable and neither it nor any object it links includes an SDL header
