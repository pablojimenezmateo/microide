## ADDED Requirements

### Requirement: The server accounts for everything it runs
`server/status` SHALL report, in addition to what it reports today, the server's uptime, resident memory and CPU share; per workspace its attached clients, open terminals, live processes, manifest file count, hashed file count and watch mode; per terminal its handle, workspace root, current directory, title, shell pid, attached clients, exited flag, scrollback line count, and the resident memory and CPU share of the shell and every process under it; per process its handle, workspace root, argv (bounded at spawn), pid, current directory, whether it is kept past a detach and since when, exited flag, buffered output bytes, resident memory and CPU share likewise. CPU share SHALL be computed from the difference between this request's `/proc` sample and the previous one for that pid, reported as not available on the first sample; the server SHALL read `/proc` only when asked and SHALL run no sampling of its own. Every added field SHALL be additive to the protocol, and the client SHALL decode each as untrusted, treating a missing or malformed field as not reported.

#### Scenario: A build under a host terminal is attributed to it
- **WHEN** a host terminal's shell runs `make -j8` and a client asks `server/status` twice two seconds apart
- **THEN** the second reply lists that terminal with a CPU share near the cores the build is using and a resident size that includes the compilers, and the terminal's row names the terminal, not the compilers

#### Scenario: An idle server does no accounting
- **WHEN** no client asks for status for an hour
- **THEN** the server reads `/proc` zero times in that hour

### Requirement: A Host view shows a connected host's activity and acts on it
The sidebar SHALL offer a contextual view `Host`, present while a host is connected, listing that host's Server, Workspaces, Terminals and Processes as groups with one row per item, each row carrying its figures (CPU share, memory, clients or kept time) precomposed once per refresh. While visible the view SHALL refresh from `server/status` every two seconds and ask at once when shown; hidden, it SHALL cause no requests. Enter on a terminal row SHALL focus the terminal tab holding that handle, or attach the terminal in a new tab when none does; Delete on a terminal or process row SHALL end it after confirmation when it is running, and SHALL instead report a terminal that another client holds; the Server row SHALL offer Stop Host Server. These actions SHALL be `ActionId`s reachable from the palette and the control channel. `Remote: Show Status` SHALL open this view, and the status bar's remote segment SHALL open it on click; the headless control channel SHALL keep a text answer that includes the host's totals. An older server that reports none of the figures SHALL leave the view showing the connection-level rows with the figures marked as not reported.

#### Scenario: A forgotten watch is found and ended
- **WHEN** a user opens the Host view on a host where a terminal closed weeks ago still runs `watch date`
- **THEN** the Terminals group lists it with its directory, its pid and a non-zero CPU share, Enter attaches it in a new tab, and Delete with confirmation ends it so the host's `pgrep watch` finds nothing

#### Scenario: A kept language server is visible with its age
- **WHEN** a remote project's `clangd` is kept after a disconnect and the user reconnects and opens the view
- **THEN** the Processes group lists `clangd` as kept with the time since the detach and its memory

#### Scenario: No cost while hidden
- **WHEN** the view is hidden for five seconds
- **THEN** no `server/status` request is sent in that time

#### Scenario: An older server
- **WHEN** the host runs a server that reports no activity fields
- **THEN** the view shows the connection, workspaces and totals it has, with figures marked not reported, and no error
