## ADDED Requirements

### Requirement: Frames are length-prefixed binary with a typed header
Every message between client and server SHALL be one frame: a fixed header carrying `length`, `type`, `lane` and a request `id`, followed by exactly `length` payload bytes. Control frames SHALL carry a JSON body; content frames (process stdio, terminal lines and screens) SHALL carry raw bytes with no text encoding step. A frame whose length exceeds the 64 MiB per-frame ceiling SHALL be rejected and SHALL end the connection rather than be buffered.

#### Scenario: A UTF-8 sequence split across two content frames reassembles exactly
- **WHEN** a process writes a multi-byte UTF-8 character whose bytes are delivered in two consecutive `proc/stdout` frames
- **THEN** the client's reassembled stream is byte-identical to what the process wrote, with no replacement characters

#### Scenario: An oversized frame ends the connection
- **WHEN** a peer sends a header announcing a payload larger than the per-frame ceiling
- **THEN** the receiver closes the connection without allocating the announced size and reports a protocol error

### Requirement: Two priority lanes and an adaptive bulk bound
Every frame SHALL carry a lane, `interactive` or `bulk`. Each sender SHALL drain interactive frames before bulk ones, and SHALL keep at most `remote.backfill_inflight_bytes` of bulk payload unacknowledged. The bound SHALL be adaptive: about 100 ms of the most recently measured bandwidth, floored at 64 KiB and capped at 1 MiB. `term/input`, `term/screen`, `proc/*` stdio, `link/ping` and replies to user-initiated requests SHALL be interactive; `term/lines` and scrollback backfill SHALL be bulk.

#### Scenario: A keystroke is not queued behind a bulk transfer
- **WHEN** a bulk transfer of at least 50 MiB is in progress on the connection and the user types in a host terminal
- **THEN** the `term/input` frame and the resulting `term/screen` frame are sent ahead of any bulk frame not yet written

### Requirement: The protocol version is separate from the app version
`server/hello` SHALL carry the client's protocol version and the minimum protocol version it accepts; the reply SHALL carry the server's protocol version, its minimum, its release version, a `daemon_epoch` unique to the server process, its capabilities, the session-survival report, and the effective values of the settings it consumes. A version outside either side's accepted range SHALL fail the handshake with a message naming both release versions.

#### Scenario: A newer client talks to an older server whose wire has not changed
- **WHEN** the client's protocol version is within the server's accepted range and vice versa, though their release versions differ
- **THEN** the handshake succeeds

#### Scenario: An incompatible server is reported, not guessed at
- **WHEN** the server's protocol version is below the client's minimum
- **THEN** the handshake fails with a message naming the server's release version and the client's, and the client proceeds to install a matching server (see `remote-connection-lifecycle`)

### Requirement: A heartbeat detects a dead link in seconds
The client SHALL send `link/ping` every 2 s on the interactive lane carrying its clock; the server SHALL answer each. Three consecutive unanswered pings SHALL declare the link dead. Both sides SHALL maintain a running round-trip estimate from the pings, which the prediction overlay and the bulk bound read.

#### Scenario: A pulled cable is noticed without waiting for ssh
- **WHEN** the transport stops delivering frames in both directions
- **THEN** the client declares the link dead within 6 s and enters Reconnecting, regardless of ssh's own keepalive settings

### Requirement: Long requests can be cancelled
`op/cancel` (a notification carrying a request id) SHALL stop an in-flight `proc/spawn` stream or `term/scrollback` request. A cancelled request SHALL produce no further frames for that id other than its terminal reply.

#### Scenario: Closing a terminal tab mid-backfill stops the backfill
- **WHEN** the client cancels a `term/scrollback` request that is still streaming
- **THEN** no further `term/lines` frames for that request arrive after the cancellation is processed

### Requirement: Per-handle credit windows bound unbounded streams
Process stdio and terminal scrollback lines SHALL be flow-controlled per handle by a credit window (`remote.term_credit_bytes`, default 256 KiB outstanding), replenished as the client acknowledges. Terminal lines produced beyond the window SHALL be dropped at the host's ring and reported as a counted gap; process stdio beyond the window SHALL be held by the server until credit returns. A runaway stream SHALL NOT tear down the connection or affect other handles.

#### Scenario: `yes` in one terminal does not stall another
- **WHEN** one host terminal runs a command producing output faster than the link drains it, and a second host terminal is typed into
- **THEN** the second terminal's echo is unaffected and the first terminal shows a visible gap with a count of dropped lines

### Requirement: Everything decoded is untrusted
Every field the client decodes from the server, and every field the server decodes from a client, SHALL be validated before use: sizes against the frame ceiling and configured limits, ids against live handles, paths against the roots they claim to belong to. The frame decoder SHALL be a fuzz target.

#### Scenario: A malformed control body is rejected without crashing
- **WHEN** a control frame's JSON body is truncated, mistyped or carries unknown handle ids
- **THEN** the receiver replies with a protocol error for that request and the connection and every other handle continue
