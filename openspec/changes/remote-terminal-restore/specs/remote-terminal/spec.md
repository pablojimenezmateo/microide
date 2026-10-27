## ADDED Requirements

### Requirement: A host terminal requested before the connection is ready waits for it
A terminal requested in a remote project, or through `Remote: Open Terminal on Host…`, while that project's or host's connection is not `Ready` SHALL be created at once as a pending pane in the terminal strip, showing the connection's state as one status line in its body, rewritten only when that state changes. The pane SHALL NOT be dropped, and input to it SHALL be dropped rather than queued. When the connection first reaches `Ready`, every pending pane of that connection in the active project SHALL start its shell (or reattach, per "Reopening reattaches each terminal to its host shell") in its recorded directory; a pending pane of an inactive project SHALL start when that project is next activated with its connection `Ready`. If the connection fails for good (`Disconnected`, `Offline`), the pending panes SHALL show that and relaunch with Enter, as an exited pane does. This applies to the project's default terminal, to revived terminals and to terminals the user opens from the `+`, the menu, the palette or `Remote: Open Terminal on Host…`, which SHALL use the same mechanism rather than a separate queue.

#### Scenario: A remote project opens with its terminal
- **WHEN** a remote project is opened and its link takes three seconds to become `Ready`
- **THEN** the terminal panel shows one terminal tab from the moment the project opens, reading as connecting, and a shell prompt from the host appears in that same tab once the link is `Ready`, with no tab added or removed

#### Scenario: A terminal opened while connecting is not lost
- **WHEN** the user clicks `+` while the remote project's link is still connecting
- **THEN** a second pending tab appears immediately and becomes a host shell when the link is `Ready`

#### Scenario: Open Terminal on Host shows its tab at once
- **WHEN** the user runs `Remote: Open Terminal on Host…` for a host that is not yet connected
- **THEN** the tab appears immediately, reading as connecting, and holds the host shell once the link is `Ready`

### Requirement: Host terminals survive quitting microide and closing the project
Quitting microide and closing a remote project's tab SHALL leave every host terminal of that window or project running on the host: the client SHALL stop receiving its frames without sending `term/close`. For close-project this SHALL be an explicit detach step ordered before the project's state is persisted and torn down, not a consequence of the order of unrelated calls. Closing a terminal tab or pane, or the shell exiting, SHALL still end the host terminal per "The pty and the terminal model live on the host". When `terminal.restore_sessions` is off, quit and close-project SHALL close host terminals instead, so a user who opted out of revival leaves nothing running.

#### Scenario: Quit leaves the host shell running
- **WHEN** a remote project's terminal is running `watch date` and the user quits microide
- **THEN** the `watch` process is still running on the host afterwards, and the server lists its terminal as attached to no client

#### Scenario: Closing the project leaves the host shell running
- **WHEN** the user closes the remote project's tab instead of quitting
- **THEN** the server still lists the terminal, attached to no client, and the shell and `watch` keep running

#### Scenario: Closing the tab still ends the shell
- **WHEN** the user closes that terminal tab instead
- **THEN** the host shell and `watch` are ended, and the server no longer lists the terminal

#### Scenario: Opted out of revival
- **WHEN** `terminal.restore_sessions` is off and the user quits, or closes the project, with a host terminal open
- **THEN** the host terminal is closed, as closing its tab would

### Requirement: Reopening reattaches each terminal to its host shell
The project session SHALL record, for each host pane, the server epoch, the host terminal handle and, for a pane opened with `Remote: Open Terminal on Host…`, the host target, in addition to the layout, directory and command it records today; a pane opened with no project open has no project session and SHALL be left to discovery (see "Host terminals nobody is attached to are discoverable"). On reopen, each recorded pane SHALL be pending until its connection is `Ready` and then SHALL cold-attach to its handle when the server's epoch equals the recorded one: the pane SHALL show the live screen plus `remote.scrollback_prefetch_lines` of history with a dim `reattached` rule beneath it, and older history fetched on scroll. When the epoch differs, or the server refuses the handle, the pane SHALL start a fresh shell in the recorded directory, and the per-host row (below) SHALL say how many terminals started fresh and why (the server restarted, or the terminal is gone, with the host's session-survival policy named when it explains the loss). A recorded terminal whose shell exited while detached SHALL reattach showing its final output and exit line, and SHALL relaunch with Enter. A session written without these records SHALL revive fresh shells as before.

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
The server SHALL answer `term/list {}` with its live terminals: handle, the workspace root of the connection that opened it (empty for a terminal-only session), current directory, title, number of attached clients and whether the shell has exited, bounded by its terminal cap. After its connection reaches `Ready` and the session's own reattaches have been sent, the client SHALL send it once and report the terminals it did not claim and no other client is attached to, under the project's root or, for a terminal-only connection, with no root. The report SHALL be one row per host that sums the connect — terminals reattached, started fresh (with the reason) and running with no tab — with **Attach** (open each unclaimed one in a new terminal tab, cold) and **Close** (end them) actions. `Remote: Show Status` SHALL list the unclaimed terminals. An older server that answers `term/list` with an unknown-method error SHALL be treated as having nothing to list, and the protocol version SHALL NOT change.

#### Scenario: A terminal left by a crash is found
- **WHEN** microide crashed while a host terminal was open, so the session never recorded its handle, and the user reopens the project
- **THEN** the host's row reports one terminal running with no tab, and Attach opens it in a new tab with its screen and history

#### Scenario: A terminal another machine is using is not offered
- **WHEN** a second machine is attached to a host terminal of the same project
- **THEN** that terminal is not reported as unclaimed

#### Scenario: An older server
- **WHEN** the host runs a server that does not know `term/list`
- **THEN** the row reports the reattaches alone and nothing is listed as unclaimed
