## ADDED Requirements

### Requirement: Every remote process is a child of the server
`proc/spawn` SHALL start a process on the host as a child of the server, from an `argv` array, a `cwd` that is a host path, environment additions, and a `keep_on_detach` flag. No shell SHALL be interposed and no argument SHALL be quoted or joined. The server SHALL report `proc/exit` with the exit status or terminating signal.

#### Scenario: An argument with spaces, quotes, `$HOME` and a newline arrives intact
- **WHEN** a client spawns `printf %s` with one argument containing a space, both quote characters, the text `$HOME` and a newline
- **THEN** the process's stdout is byte-identical to that argument

### Requirement: Process stdio is resumable
Each stdio stream of a spawned process SHALL carry a byte offset. A reattaching client SHALL state the offset it has through for each stream of each kept handle, and the server SHALL send from that offset with nothing duplicated and nothing skipped. `proc/stdin` and `proc/signal` SHALL be accepted for a live handle.

#### Scenario: A build keeps building through a dropped link
- **WHEN** a process spawned with `keep_on_detach` is producing output and the link dies, and the client reattaches a minute later
- **THEN** the process was never signalled, and the client receives its stdout from the last acknowledged offset with no duplicate bytes

#### Scenario: A process that exited while detached reports its exit
- **WHEN** a kept process exits while no client is attached
- **THEN** its remaining output and its exit status are delivered on the next attach, and the handle is reaped only after a client has received them

### Requirement: Processes not kept on detach are stopped
A process spawned without `keep_on_detach` SHALL be terminated when the client that spawned it detaches or the link is declared dead.

#### Scenario: A language server does not outlive its client
- **WHEN** a client with a running language server spawned through `proc/spawn` disconnects
- **THEN** the language server is terminated on the host

### Requirement: RemoteProcessLauncher is the project launcher of a host session
`RemoteProcessLauncher` SHALL implement `platform::ProcessLauncher`: `Run` as a `proc/spawn` that collects output to completion, `ResolveArgv` as the identity, `ResolveWorkingDirectory` and `LocalPathFromHost` as the session's two-way path map, and `is_local()` as false. It SHALL also implement `project::GitMetadataSource` from the host's answer. Every existing spawn site SHALL run on the host through it without call-site changes.

#### Scenario: Git runs on the host through the project's launcher
- **WHEN** a git command is run through a project whose launcher is a `RemoteProcessLauncher`
- **THEN** exactly one `proc/spawn` reaches the server, with git's argv and the host working directory, and the result is returned as from a local spawn

#### Scenario: A long-lived process starts through the same route
- **WHEN** the language server or debug adapter of such a project starts
- **THEN** it is a `proc/spawn` on the host whose stdio is carried by the protocol, and the JSON-RPC transport's path translation applies to its messages
