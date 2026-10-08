## ADDED Requirements

### Requirement: The pty and the terminal model live on the host
`term/open {cwd, shell, rows, cols}` SHALL create a pty on the host owned by the server and run the shell in it, with the terminal model parsing its output on the host. `term/resize` SHALL resize it; `term/close` SHALL shut it down. A dropped link SHALL NOT signal the shell or anything it runs.

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
