## ADDED Requirements

### Requirement: A kept process nobody reattaches to is released after the keep grace
A process spawned with `keep_on_detach` SHALL survive its client's detach only for the server's keep grace (`--keep-grace-ms`, default 30 minutes), so a client that lost its link takes it back with its state. When no client has reattached by then the server SHALL terminate it (SIGTERM, then SIGKILL) and forget its handle exactly as for an unkept process. The grace SHALL be passed through `attach` to the on-demand server it starts.

#### Scenario: A language server left behind by a killed client
- **WHEN** a client with a running language server is killed and no client reattaches within the grace
- **THEN** the language server is terminated on the host and `proc/attach` for its handle is refused

#### Scenario: A reconnect within the grace keeps it
- **WHEN** the client reattaches the handle before the grace runs out
- **THEN** the process is still running and the grace no longer applies to it
