## ADDED Requirements

### Requirement: Remote terminal and link budgets
The perf harness SHALL measure, against a local `microide-server` with injected latency (`--server-delay-ms`), and enforce:
- host terminal open to first prompt within 1 RTT plus shell startup;
- a typed glyph painted in the keystroke's frame while prediction is active;
- confirmed echo within 1 RTT + 5 ms while a 50 MiB bulk transfer runs on the same connection;
- a contradicted prediction removed within 1 RTT;
- a dead link detected (Ready to Reconnecting) within 6 s;
- reattach to a server with three live terminals painting each within 2 RTT, with the visible screen and 500 lines;
- no shell-thread frame over 16 ms while the server is stalled.
These are time budgets and SHALL run only in the perf harness, one scenario per process.

#### Scenario: Echo under a bulk transfer
- **WHEN** the harness injects 80 ms of latency and runs a 50 MiB bulk transfer while typing in a host terminal
- **THEN** every confirmed echo arrives within 85 ms of its keystroke

### Requirement: Round trips are budgeted by count in the parity suite
The parity suite SHALL run every scenario against the local launcher, the loopback launcher and the real server over a pipe, and SHALL fail any non-local run that starts more host processes than the local run of the same scenario. These checks SHALL be deterministic and SHALL run in the ordinary sharded test suite.

#### Scenario: A remote path that spawns twice where local spawns once
- **WHEN** a code change makes the server locality start two processes for work the local locality does with one
- **THEN** the parity row fails and prints both spawn logs
