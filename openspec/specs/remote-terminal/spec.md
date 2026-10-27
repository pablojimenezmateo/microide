
## Purpose

Define host terminals: a pty and terminal model on the host, screen-state frames to the client, semantic input, predicted echo, scrollback and multi-client sharing.

## Requirements

### Requirement: The pty and the terminal model live on the host
`term/open {cwd, shell, rows, cols}` SHALL create a pty on the host owned by the server and run the shell in it, with the terminal model parsing its output on the host. `term/resize` SHALL resize it; `term/close` SHALL shut it down — unless another client is attached, when it SHALL detach only the closing client (closing a tab on one machine does not end a shell someone is using on another). A dropped link SHALL NOT signal the shell or anything it runs.

#### Scenario: A build in a host terminal survives a lid close
- **WHEN** a host terminal is running a long command and the client machine suspends for ten minutes
- **THEN** the command is still running on resume and its output since the suspension is shown after reattach

### Requirement: The wire carries screen state and scrollback lines, not the byte stream
The host SHALL send `term/screen` deltas of the visible screen, coalesced to at most one per 16 ms (33 ms while the handle's credit window is more than half full), plus completed scrollback lines as `term/lines` under the per-handle credit window. Each `term/screen` SHALL carry `echo_ack` and the capture bits the client needs locally (mouse capture, motion capture, focus events, cursor shape and visibility).

#### Scenario: A loud build costs the link only visible change
- **WHEN** a command in a host terminal writes 10 MB/s of output
- **THEN** the wire carries screen deltas and credit-windowed lines, never the raw output, and the client's frame time is unaffected

### Requirement: Input is semantic and encoded on the host
`term/input` SHALL carry key presses (keysym plus modifiers), pastes (text) and mouse events (button, action, cell) as semantic events, each with a monotonically increasing `input_seq`. The host SHALL encode them using its own current mode state (cursor-key mode, Kitty keyboard flags, bracketed paste, mouse tracking and encoding, size).

#### Scenario: An arrow key pressed as vim exits reaches the shell correctly
- **WHEN** the user presses an arrow key in the same instant a full-screen program leaves application-cursor mode
- **THEN** the byte sequence written to the pty is the one for the mode the host is in when it encodes the key, not the client's one-round-trip-old view

### Requirement: The terminal's outward signals reach the client
`term/event` SHALL carry the window title, the bell, OSC 7 working-directory reports (as host paths, mapped by the client), and OSC 52 clipboard writes. A clipboard write SHALL be applied by the client under the same policy a local terminal uses.

#### Scenario: A program sets the tab title
- **WHEN** a program in a host terminal sets the title with OSC 0
- **THEN** the client's terminal tab shows that title

### Requirement: Echo is predicted locally and validated by echo_ack
`TerminalPredictionOverlay` SHALL be a pure value type over the rendered lines, the typed events with their `input_seq`, the confirmed screens with their `echo_ack`, and the RTT estimate. It SHALL predict a printable character at the cursor and a backspace, draw pending predictions underlined, confirm a prediction when a frame with `echo_ack >= seq` shows the predicted cell, and drop a contradicted prediction and stop predicting until a confirmed frame agrees again. It SHALL predict nothing for control keys, at the right margin, while the cursor is hidden, or on the alternate screen until the program has shown it echoes. `remote.predict` SHALL be `adaptive` (predict only while the RTT estimate exceeds 30 ms) by default, with `always` and `never`. The host SHALL set `echo_ack` to the latest input written to the pty once the pty has produced output since that write, or after 50 ms otherwise.

#### Scenario: A typed glyph appears in the keystroke's frame
- **WHEN** the RTT estimate is 120 ms and the user types `a` at a shell prompt
- **THEN** `a` is painted, underlined, in the frame that handles the keystroke, and the confirmed frame replaces it with no visible change

#### Scenario: A wrong prediction is gone within a round trip
- **WHEN** the user types a character the program does not echo (a password prompt)
- **THEN** the predicted glyph disappears when the next frame with a covering `echo_ack` arrives, and no further predictions are drawn until a confirmed frame agrees

#### Scenario: A mode change during a burst leaves the confirmed screen exact
- **WHEN** the host enters the alternate screen between two predicted keystrokes
- **THEN** the screen after the next confirmed frame equals that frame byte for byte, with no leftover predicted cells

### Requirement: The host holds the authoritative scrollback and attach resumes from an offset
Each host terminal SHALL keep its scrollback on the host, sized by `terminal.scrollback_lines`. Attach SHALL deliver the visible screen plus `remote.scrollback_prefetch_lines` (default 500) immediately and older history lazily through `term/scrollback`, keyed by the trim-total offset, with nothing duplicated. Lines dropped at the ring, by overflow while detached or by the credit window, SHALL be shown as a rule with the count. A shell that exited while detached SHALL keep its final output and exit status until a client has seen them.

#### Scenario: Reattach from a different machine shows history
- **WHEN** a client with no local scrollback attaches to a terminal with 5,000 lines of history
- **THEN** the screen and the last 500 lines paint within two round trips, and scrolling up fetches older pages without blocking the shell thread

#### Scenario: Reattach with a partial buffer duplicates nothing
- **WHEN** a client that already holds lines up to offset N reattaches
- **THEN** it receives lines from N onward only

### Requirement: Two clients share one pty at the smaller size
When two clients are attached to one terminal, the pty SHALL take the smaller of their sizes in each dimension, and each client SHALL draw the unused margin as such.

#### Scenario: A second client with a smaller window
- **WHEN** a second client attaches with fewer columns than the first
- **THEN** the pty is resized to the smaller width and both clients see the same output

### Requirement: A host terminal requested before the connection is ready waits for it
A terminal requested in a remote project while that project's connection is not `Ready` SHALL be created at once as a pending pane in the terminal strip, showing the connection's state as one status line in its body, rewritten only when that state changes. The pane SHALL NOT be dropped, and input to it SHALL be dropped rather than queued. When the connection first reaches `Ready`, every pending pane of that connection in the active project SHALL start its shell (or reattach, per "Reopening reattaches each terminal to its host shell") in its recorded directory; a pending pane of an inactive project SHALL start when that project is next activated with its connection `Ready`. If the connection fails for good (`Disconnected`, `Offline`), the pending panes SHALL show that and relaunch with Enter, as an exited pane does. This applies to the project's default terminal, to revived terminals and to terminals the user opens from the `+`, the menu or the palette.

#### Scenario: A remote project opens with its terminal
- **WHEN** a remote project is opened and its link takes three seconds to become `Ready`
- **THEN** the terminal panel shows one terminal tab from the moment the project opens, reading as connecting, and a shell prompt from the host appears in that same tab once the link is `Ready`, with no tab added or removed

#### Scenario: A terminal opened while connecting is not lost
- **WHEN** the user clicks `+` while the remote project's link is still connecting
- **THEN** a second pending tab appears immediately and becomes a host shell when the link is `Ready`

### Requirement: Host terminals survive quitting microide and closing the project
Quitting microide and closing a remote project's tab SHALL leave every running host terminal of that window or project running on the host: the client SHALL stop receiving its frames without sending `term/close`. In both cases this SHALL be one explicit detach step — over the closing project's terminals before its state is persisted and torn down, and over every open project's terminals (active or not) before quit persists — not a consequence of the order of unrelated calls or of how the process exits, so the test binaries, which run destructors at quit, behave as the shipping binary does. Closing a terminal tab or pane, or the shell exiting, SHALL still end the host terminal per "The pty and the terminal model live on the host". Only a terminal whose shell is still running SHALL be left on the host: one whose shell has already exited while this client was attached SHALL be closed by quit and by close-project, so that no process-less buffer is left holding the server open, and its tab SHALL revive as a fresh shell. When `terminal.restore_sessions` is off, quit and close-project SHALL close every host terminal instead, so a user who opted out of revival leaves nothing running.

#### Scenario: Quit leaves the host shell running
- **WHEN** a remote project's terminal is running `watch date`, a second remote project that is not the active tab also has a terminal, and the user quits microide
- **THEN** both shells are still running on the host afterwards, and the server lists both terminals as attached to no client

#### Scenario: Closing the project leaves the host shell running
- **WHEN** the user closes the remote project's tab instead of quitting
- **THEN** the server still lists the terminal, attached to no client, and the shell and `watch` keep running

#### Scenario: Closing the tab still ends the shell
- **WHEN** the user closes that terminal tab instead
- **THEN** the host shell and `watch` are ended, and the server no longer lists the terminal

#### Scenario: Exited shells leave nothing behind
- **WHEN** the user ends each of three host shells with Ctrl+D, leaves their tabs open, and quits microide
- **THEN** the server lists no terminal, and an on-demand server with no other workspace idles out

#### Scenario: Opted out of revival
- **WHEN** `terminal.restore_sessions` is off and the user quits, or closes the project, with a host terminal open
- **THEN** the host terminal is closed, as closing its tab would

### Requirement: Reopening reattaches each terminal to its host shell
The project session SHALL record, for each host pane, the server epoch and the host terminal handle, in addition to the layout, directory and command it records today. On reopen, each such pane SHALL be pending until its connection is `Ready` and then SHALL cold-attach to its handle when the server's epoch equals the recorded one: the pane SHALL show the live screen plus `remote.scrollback_prefetch_lines` of history with a dim `reattached` rule beneath it, and older history fetched on scroll. When the epoch differs, or the server refuses the handle, the pane SHALL start a fresh shell in the recorded directory, and the per-host row (below) SHALL say how many terminals started fresh and why (the server restarted, or the terminal is gone, with the host's session-survival policy named when it explains the loss). A recorded terminal whose shell exited while detached SHALL reattach showing its final output and exit line, and SHALL relaunch with Enter. A session written without these records SHALL revive fresh shells as before.

#### Scenario: watch date comes back
- **WHEN** the user runs `watch date` in a remote project's terminal, quits microide, waits a minute and reopens it
- **THEN** the same tab, once the link is `Ready`, shows `watch` still updating with the current time, and scrolling up shows the lines printed before the quit

#### Scenario: The host server restarted in between
- **WHEN** the host server was stopped and started again between the quit and the reopen
- **THEN** the tab starts a fresh shell in the recorded directory, and the host's row says one terminal started fresh because the host server restarted

#### Scenario: A build finished while microide was closed
- **WHEN** a host terminal's build finished and its shell exited while microide was closed
- **THEN** on reopen the tab shows the build's last output and the exit line, and Enter starts a new shell in the same directory

### Requirement: Host terminals nobody is attached to are discoverable
The `server/status` reply SHALL list the server's live terminals — handle, the workspace root of the connection that opened it (empty for a terminal-only session), current directory, number of attached clients and whether the shell has exited — bounded by its terminal cap, and its per-workspace terminal count SHALL be derived from that list. After its connection reaches `Ready` and the session's own reattaches have been sent, the client SHALL ask once and report the terminals it did not claim and no other client is attached to, under the project's root. The report SHALL be one row per host that sums the connect — terminals reattached, started fresh (with the reason) and running with no tab — with **Attach** (open each unclaimed one in a new terminal tab, cold) and **Close** (end them) actions, each an `ActionId` taking the host and the handles so the palette, the control channel and later surfaces reach them. `Remote: Show Status` SHALL list the unclaimed terminals. The client SHALL decode the listing as untrusted, a missing or malformed field reading as not reported; an older server whose reply has no listing SHALL be treated as having nothing to list, and the protocol version SHALL NOT change.

#### Scenario: A terminal left by a crash is found
- **WHEN** microide crashed while a host terminal was open, so the session never recorded its handle, and the user reopens the project
- **THEN** the host's row reports one terminal running with no tab, and Attach opens it in a new tab with its screen and history

#### Scenario: A terminal another machine is using is not offered
- **WHEN** a second machine is attached to a host terminal of the same project
- **THEN** that terminal is not reported as unclaimed

#### Scenario: An older server
- **WHEN** the host runs a server whose `server/status` reply carries no terminal listing
- **THEN** the row reports the reattaches alone and nothing is listed as unclaimed
