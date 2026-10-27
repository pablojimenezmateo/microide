## MODIFIED Requirements

### Requirement: Subcommands start, stop, status and attach
The binary SHALL provide `start` (daemonize and stay up), `stop`, `status` and `attach` (connect to the running server, or start one, then relay stdio to it). `status` SHALL print, as a readable table by default and as JSON with `--json`, the socket, the server's pid, uptime, memory and CPU share, and each workspace with its attached clients, its terminals (directory, title, pid, clients, memory, CPU share of the shell and what it runs) and its processes (argv, pid, kept state, memory, CPU share); the table form SHALL take the two samples a CPU share needs itself. A server started with `start` SHALL never exit for idleness; one started on demand by `attach` SHALL exit when it has no workspaces and its idle timer expires.

#### Scenario: A hand-started server survives a night with no clients
- **WHEN** a user runs `microide-server start`, connects, and disconnects for longer than the idle timeout
- **THEN** the server is still running and the next `attach` reaches it without a restart

#### Scenario: An on-demand server exits when idle
- **WHEN** `attach` started the server and every workspace has closed and the idle timeout passes
- **THEN** the server exits and removes its socket

#### Scenario: status answers "what did I leave running"
- **WHEN** a user runs `microide-server status` on a host with one workspace holding a terminal running `watch date` and a kept `clangd`
- **THEN** the table shows that workspace with the terminal's directory, pid and CPU share and the process's argv, kept state and memory, and `--json` prints the same as `server/status` returns
