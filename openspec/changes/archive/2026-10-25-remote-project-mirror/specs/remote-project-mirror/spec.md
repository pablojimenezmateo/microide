## ADDED Requirements

### Requirement: The mirror is the host's content set
Opening `[user@]host[:port]:/path` SHALL open a local mirror of the host directory's content set (tracked and untracked non-ignored files, one repository deep plus checked-out submodules) as the project root. Once the sync reports Ready the mirror SHALL be byte-for-byte equal to the host's content set, and a change the host makes afterwards SHALL reach the mirror within one round trip plus the 250 ms coalesce window plus the pull of its bytes. The sync's progress SHALL be a sticky notification row keyed by the mirror root that is withdrawn at Ready.

#### Scenario: First open of a repository
- **WHEN** the user opens a 4,500-file repository on a host ~55 ms away with no mirror yet
- **THEN** the project tab and sidebar appear at once, the row counts down the files to fetch, and when it is withdrawn one SHA-256 over every file of the content set is equal on both sides

#### Scenario: A host process writes a file
- **WHEN** a process on the host rewrites a tracked file (truncate-in-place or temp-and-rename)
- **THEN** the mirror's copy holds the new bytes within about half a second on that link, without any action in the editor

### Requirement: A save lands locally and is pushed under compare-and-swap
A save in a remote project SHALL write the mirror first and return, then push the bytes to the host with the mirror's recorded base hash as the precondition. The push SHALL never overwrite a host version the mirror had not seen. While the project is disconnected (deliberately or by a dead link) a save SHALL still succeed locally, be journaled, and be replayed in order on the next Ready.

#### Scenario: Save on a connected project
- **WHEN** the user edits a clean file and saves
- **THEN** the tab is clean at once and the host file holds the bytes within two round trips

#### Scenario: Save while disconnected
- **WHEN** the user runs `remote-disconnect`, edits a file and saves, then `remote-reconnect`
- **THEN** the save succeeds locally with the tab clean, and the host file holds the bytes within two round trips of the reconnect

#### Scenario: Save during a dead link
- **WHEN** the ssh master is killed and the user saves before the client has reconnected
- **THEN** a Reconnecting row with a Reconnect button appears, the client reconnects by itself, and the host file holds the save afterwards

### Requirement: A host change under an open tab follows the editor's external-change contract
When the mirror pulls a new version of a file that is open and CLEAN, the buffer SHALL reload silently and the change SHALL be counted into the aggregated "N files changed on host" notification. When the file is open and DIRTY, the buffer SHALL be kept, a `save` SHALL be refused naming the banner, and the editor's changed-on-disk banner SHALL offer Compare (the host's version beside the unsaved buffer in a compare tab), Reload (take the host's) and Overwrite (push the buffer). Only a file that is open and dirty notifies individually; everything else is aggregated.

#### Scenario: Host edit under a clean tab
- **WHEN** the host prepends a line to a file open in a clean tab
- **THEN** the buffer shows the line without a prompt, "1 file changed on <host>" is posted, and a later save keeps both the host's line and the user's

#### Scenario: Host edit under a dirty tab, user keeps theirs
- **WHEN** the host changes a file the user has unsaved edits in and the user saves
- **THEN** the save is refused with a message naming the banner, Compare opens `<file> (on disk) ↔ <file> (your changes)`, and Overwrite pushes the buffer so the host holds the user's version and the tab is clean

#### Scenario: Host edit under a dirty tab, user takes the host's
- **WHEN** the same conflict is answered with Reload
- **THEN** the buffer holds the host's version, the tab is clean, and the next save pushes normally

### Requirement: A pushed version the host changed meanwhile is a parked conflict
When a journaled or in-flight push finds the host's version differs from the base (the host changed the file while the save was unsent), the push SHALL be parked, the mirror SHALL keep the user's bytes, and a sticky conflict row per file SHALL offer Keep Mine, Take Host's and Merge (a three-way merge editor from the common base). `remote-resolve <path> <mine|host|compare|merge>` SHALL be the command form of those buttons. A parked conflict SHALL survive a restart of the editor.

#### Scenario: Offline save meets a host edit
- **WHEN** the user saves while disconnected, the host changes the same file, and the client reconnects
- **THEN** `<file> changed on <host> and here` is posted with Keep Mine, Take Host's and Merge, and the host file is unchanged until one is chosen

### Requirement: A mass delete is held unless the host moved HEAD
A watch delta or manifest that would delete more than `remote.mass_delete_threshold` of the mirror SHALL be held, the tree untouched, with a prompt naming the count that offers Apply and Disconnect. A branch switch on the host (`watch/moved`) SHALL apply its deletes without the prompt.

#### Scenario: Checkout of an old branch
- **WHEN** the host checks out a commit that removes a third of the tree
- **THEN** the mirror follows within seconds with no prompt

#### Scenario: A host directory vanishes
- **WHEN** the host's tree loses most of its files with no HEAD move (an unmounted disk)
- **THEN** the mirror keeps its files and the prompt offers Apply and Disconnect

### Requirement: Changed on Host lists what the host changed
The sidebar SHALL offer a Changed on Host view (`remote-changes`) listing the files the host changed since the user last cleared it, each comparable against the version the mirror held before, and `remote-clear-changes` SHALL empty it. The "N files changed on host" row's Show button SHALL open it.

#### Scenario: Agents rewrite files while the user works
- **WHEN** host processes change 40 files over a minute
- **THEN** one row reports the count (replaced in place as it grows), Show opens the view with the 40 paths, and opening one compares the before and after

### Requirement: Reopening the mirror reconnects it
The mirror tree SHALL carry a record of its host and host root beside it, and opening that tree from the recents list, a restored session or the command line SHALL reconnect it as the remote project rather than open it as a local folder. Only the mirror's own tree in its own data directory SHALL be recognized; a copy or a moved directory is a local folder.

#### Scenario: Restart of the editor
- **WHEN** the editor is relaunched on the mirror path with the same data directory
- **THEN** it connects, "Connected to <host>:<path>" is posted, and a save pushes within two round trips

### Requirement: Host processes run on the host
Terminals, language servers, debug adapters, formatters, plugin tools and git of a remote project SHALL run on the host through the project's launcher, with host paths translated at the transport, and SHALL take no more host round trips than the same scenario takes processes locally.

#### Scenario: Terminal
- **WHEN** the user opens a terminal in a remote project and runs `hostname`
- **THEN** the host's name is printed

#### Scenario: Go to definition
- **WHEN** clangd runs on the host and the user asks for the definition of a function defined in the same file
- **THEN** the caret lands on the definition; cross-file navigation follows the host's `compile_commands.json`, and its absence is reported by the plugin, not silently

### Requirement: A mirror can be removed from the editor
`Remote: Remove Mirror` (`remote-remove-mirror [root]`) SHALL delete a remote project's mirror — its tree, objects, journal and state — and its recents entry, after the dirty prompt for its open tabs and never while a push is journaled and unsent without asking. The command SHALL be available for the active remote project and for a closed one named by its root.

#### Scenario: Removing the active project's mirror
- **WHEN** the user runs Remove Mirror on a connected project with one dirty tab
- **THEN** the dirty prompt runs first, the project tab closes, the mirror directory is gone, and the recents list no longer offers it

#### Scenario: Unsent pushes
- **WHEN** the project has two journaled saves the host has not received
- **THEN** the command asks before discarding them, naming the count

### Requirement: The control channel reports remote state
The `status` query SHALL carry, for a remote active project, `remote: {host, hostRoot, state, filesPending, bytesPending, unsentPushes, conflicts}` with `state` one of the lifecycle's connection states plus `syncing` and `ready`, and the `projects` query SHALL mark each remote project with its `host` and `hostRoot`. A driver SHALL be able to wait for `state == "ready"` instead of reading a notification's text.

#### Scenario: A driver waits for the sync
- **WHEN** a client polls `status` after `remote-open`
- **THEN** it sees `connecting`, `starting`, `syncing` with a falling `filesPending`, then `ready`, and `unsentPushes` rises by one on an offline save and falls to zero after the reconnect
