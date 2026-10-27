## ADDED Requirements

### Requirement: A host terminal requested before the connection is ready waits for it
A terminal requested in a remote project, or through `Remote: Open Terminal on Host…`, while that project's or host's connection is not `Ready` SHALL be created at once as a pending pane in the terminal strip, showing the connection state in its body. The pane SHALL NOT be dropped. When the connection first reaches `Ready`, every pending pane SHALL start its shell (or reattach, per "Host terminals survive quitting microide") in its recorded directory. If the connection fails for good (`Disconnect`), the pending panes SHALL show that and relaunch with Enter, as an exited pane does. This applies to the project's default terminal, to revived terminals and to terminals the user opens from the `+`, the menu or the palette.

#### Scenario: A remote project opens with its terminal
- **WHEN** a remote project is opened and its link takes three seconds to become `Ready`
- **THEN** the terminal panel shows one terminal tab from the moment the project opens, reading as connecting, and a shell prompt from the host appears in that same tab once the link is `Ready`, with no tab added or removed

#### Scenario: A terminal opened while connecting is not lost
- **WHEN** the user clicks `+` while the remote project's link is still connecting
- **THEN** a second pending tab appears immediately and becomes a host shell when the link is `Ready`

### Requirement: Host terminals survive quitting microide and closing the project
Quitting microide, and closing a remote project's tab, SHALL detach every host terminal of that window or project: the client SHALL stop receiving its frames without sending `term/close`, and the shell and everything it runs SHALL keep running on the host. This SHALL be an explicit step ordered before any connection or terminal state is torn down, not a consequence of destructor order. Closing a terminal tab or pane, or the shell exiting, SHALL still end the host terminal per "The pty and the terminal model live on the host". When `terminal.restore_sessions` is off, quit and project close SHALL close host terminals instead of detaching them.

#### Scenario: Quit leaves the host shell running
- **WHEN** a remote project's terminal is running `watch date` and the user quits microide
- **THEN** the `watch` process is still running on the host afterwards, and the server lists its terminal as attached to no client

#### Scenario: Closing the tab still ends the shell
- **WHEN** the user closes that terminal tab instead of quitting
- **THEN** the host shell and `watch` are ended, and the server no longer lists the terminal

#### Scenario: Opted out of revival
- **WHEN** `terminal.restore_sessions` is off and the user quits with a host terminal open
- **THEN** the host terminal is closed, as closing its tab would

### Requirement: Reopening reattaches each terminal to its host shell
The session SHALL record, for each host pane, the server epoch, the host terminal handle and, for a pane opened with `Remote: Open Terminal on Host…`, the host target, in addition to the layout, directory and command it records today. On reopen, each such pane SHALL be pending until its connection is `Ready` and then SHALL cold-attach to its handle when the server's epoch equals the recorded one: the pane SHALL show the live screen plus `remote.scrollback_prefetch_lines` of history, with older history fetched on scroll. When the epoch differs, or the server refuses the handle, the pane SHALL start a fresh shell in the recorded directory, and a single notification row SHALL state how many terminals could not be reattached on that host and why (the server restarted, or the terminal is gone). A recorded terminal whose shell exited while detached SHALL reattach showing its final output and exit line, and SHALL relaunch with Enter. Open Terminal on Host panes SHALL be recorded and revived like a project's panes; today they are dropped from the session.

#### Scenario: watch date comes back
- **WHEN** the user runs `watch date` in a remote project's terminal, quits microide, waits a minute and reopens it
- **THEN** the same tab, once the link is `Ready`, shows `watch` still updating with the current time, and scrolling up shows the lines printed before the quit

#### Scenario: The host server restarted in between
- **WHEN** the host server was stopped and started again between the quit and the reopen
- **THEN** the tab starts a fresh shell in the recorded directory, and one notification row says that one terminal could not be reattached because the host server restarted

#### Scenario: A build finished while microide was closed
- **WHEN** a host terminal's build finished and its shell exited while microide was closed
- **THEN** on reopen the tab shows the build's last output and the exit line, and Enter starts a new shell in the same directory

### Requirement: Host terminals nobody is attached to are discoverable
The server SHALL answer `term/list {}` with its live terminals: handle, workspace root (empty for a terminal-only session), current directory, title, number of attached clients, whether the shell has exited, and the time of its last output. After its connection reaches `Ready` and the session's own reattaches have been sent, the client SHALL report host terminals that it did not claim and that no other client is attached to, under the project's root or, for a terminal-only connection, with no root. It SHALL report them in one notification row per host, with **Attach** (open each one in a new terminal tab) and **Close** (end them) actions. `Remote: Show Status` SHALL list them. An older server that answers `term/list` with an unknown-method error SHALL be treated as having nothing to list.

#### Scenario: A terminal left by a crash is found
- **WHEN** microide crashed while a host terminal was open, so the session never recorded its handle, and the user reopens the project
- **THEN** a row reports one host terminal not shown in any tab, and Attach opens it in a new tab with its screen and history

#### Scenario: A terminal another machine is using is not offered
- **WHEN** a second machine is attached to a host terminal of the same project
- **THEN** that terminal is not reported as unclaimed
