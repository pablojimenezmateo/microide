## ADDED Requirements

### Requirement: The server accounts for everything it runs
`server/status` SHALL report, in addition to what it reports today and to the terminal listing, per workspace its attached clients, open terminals, live processes, manifest file count, hashed file count and watch mode; per terminal its shell pid and scrollback line count; per process its handle, workspace root, argv (bounded at spawn), pid, current directory, whether it is kept past a detach and since when, exited flag and buffered output bytes. When asked with `activity`, or in a subscription push, the reply SHALL also carry the server's uptime, resident memory and CPU share, and per terminal and per process the resident memory and CPU share of the pid and every process under it, from one `/proc` sample; CPU share SHALL be the difference between this sample and the previous one for that pid, reported as not available on the first sample. The server SHALL sample only for a request that asks for activity or while at least one connection is subscribed to `activity/subscribe`, pushing `activity/changed` to subscribers on a fixed cadence and stopping with the last unsubscribe or connection close; an unsubscribed, unasked server SHALL read `/proc` not at all. Every added field SHALL be additive to the protocol, and the client SHALL decode each as untrusted, treating a missing or malformed field as not reported.

#### Scenario: A build under a host terminal is attributed to it
- **WHEN** a host terminal's shell runs `make -j8` and a client is subscribed to activity
- **THEN** the second push lists that terminal with a CPU share near the cores the build is using and a resident size that includes the compilers, and the terminal's row names the terminal, not the compilers

#### Scenario: An idle server does no accounting
- **WHEN** no client is subscribed and none asks for activity for an hour
- **THEN** the server reads `/proc` zero times in that hour

#### Scenario: The last subscriber stops the sampling
- **WHEN** the only subscribed client unsubscribes, or its connection closes
- **THEN** no further push is sent and the sampler stops

### Requirement: A Host view shows a connected host's activity and acts on it
The sidebar SHALL offer a contextual view `Host`, present while a host is connected and reached from `Remote: Show Status`, listing that host's Server, Workspaces, Terminals and Processes as groups with one row per item, each row carrying its figures (CPU share, memory, clients or kept time) precomposed once per push. Showing the view SHALL subscribe to the host's activity and hiding it SHALL unsubscribe, so a hidden view causes no requests and no pushes. Enter on a terminal row SHALL focus the terminal tab holding that handle, or attach the terminal in a new tab when none does; Delete on a terminal or process row SHALL end it after confirmation when it is running, and SHALL instead report a terminal that another client holds; the Server row SHALL offer Stop Host Server. These actions SHALL be `ActionId`s reachable from the palette and the control channel, the terminal ones being the attach and close actions the reattach row already uses. `Remote: Show Status` SHALL open this view; the status bar's remote segment keeps opening the Remote menu; the headless control channel SHALL keep a text answer that includes the host's totals. An older server that reports none of the figures SHALL leave the view showing the connection-level and terminal rows with the figures marked as not reported.

#### Scenario: A forgotten watch is found and ended
- **WHEN** a user opens the Host view on a host where a terminal closed weeks ago still runs `watch date`
- **THEN** the Terminals group lists it with its directory, its pid and a non-zero CPU share, Enter attaches it in a new tab, and Delete with confirmation ends it so the host's `pgrep watch` finds nothing

#### Scenario: A kept language server is visible with its age
- **WHEN** a remote project's `clangd` is kept after a disconnect and the user reconnects and opens the view
- **THEN** the Processes group lists `clangd` as kept with the time since the detach and its memory

#### Scenario: No cost while hidden
- **WHEN** the view is hidden for five seconds
- **THEN** no activity request or push crosses the wire in that time

#### Scenario: An older server
- **WHEN** the host runs a server that reports no activity fields and does not know `activity/subscribe`
- **THEN** the view shows the connection, workspaces, totals and terminals it has, with figures marked not reported, and no error
