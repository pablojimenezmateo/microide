## ADDED Requirements

### Requirement: Remote terminal and link budgets
The perf harness SHALL measure, against a local `microide-server` reached through a delaying, bandwidth-limited link (`tests/support/DelayRelay.h`; the scenarios in `tests/perf/RemotePerfScenarios.cpp`), and enforce:
- host terminal open to first prompt within 1 RTT plus shell startup;
- a typed glyph painted in the keystroke's frame while prediction is active;
- confirmed echo within 1 RTT + 5 ms + the one bulk chunk already on the wire (64 KiB at the link's rate) while a 50 MiB bulk transfer runs on the same connection;
- a contradicted prediction removed within 1 RTT plus the host's 50 ms echo fallback;
- a dead link detected within 6 s plus one heartbeat interval (three missed pings, the first of which may be up to one interval away);
- reattach to a server with three live terminals painting each within 2 RTT, with the visible screen and 500 lines;
- no shell-thread frame over 16 ms while the server is stalled.
These are time budgets and SHALL run only in the perf harness, one scenario per process.

#### Scenario: Echo under a bulk transfer
- **WHEN** the harness runs a 60 ms, 100 Mbit/s link and a 50 MiB bulk transfer while typing in a host terminal
- **THEN** the median confirmed echo arrives within 1 RTT + 5 ms + one 64 KiB chunk (~70 ms), and the 95th percentile within one more 16 ms frame

### Requirement: Round trips are budgeted by count in the parity suite
The parity suite SHALL run every scenario against the local launcher, the loopback launcher and the real server over a pipe, and SHALL fail any non-local run that starts more host processes than the local run of the same scenario. These checks SHALL be deterministic and SHALL run in the ordinary sharded test suite.

#### Scenario: A remote path that spawns twice where local spawns once
- **WHEN** a code change makes the server locality start two processes for work the local locality does with one
- **THEN** the parity row fails and prints both spawn logs
