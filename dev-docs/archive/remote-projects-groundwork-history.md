# Remote projects: groundwork and phase history (moved from active-work.md, 2026-11-14)

This is section 7 of `dev-docs/project/active-work.md` as it stood on 2026-11-14,
moved here verbatim: remote projects have shipped (Phases 1-3, the groundwork
G1-G11, `ProjectId`), so the dated record of how they got there is history, not
current direction. The live specs are `openspec/specs/remote-*`; the design is
`dev-docs/design/remote-projects.md`.

### 7. Remote-projects groundwork

> **Host terminals survive a quit (2026-10-27, remote-terminal-restore).** A remote
> project's terminals exist from the moment it opens (pending until the link is
> Ready, then started); quit and close-project detach running host shells in one
> explicit step (`TerminalPanelService::ReleaseHostTerminals`) instead of killing
> or orphaning them depending on how the process ended; the session records each
> pane's host terminal and a reopen reattaches it cold (or starts fresh, said so,
> when the server restarted). `server/status` lists terminals, and each connect
> reports the ones nobody holds in one row with Attach / Close. Opted out
> (`terminal.restore_sessions` off), quit closes them. See
> `openspec/changes/archive/2026-10-27-remote-terminal-restore`.
>
> **What a host runs, from the client (2026-10-27, remote-host-activity).**
> `Remote: Show Status` opens a Host sidebar view -- Server, Workspaces,
> Terminals, Processes, each row with its CPU share and memory (the pid and
> everything under it) -- pushed by the host every 2 s while the view is shown and
> never otherwise; Enter focuses or attaches a terminal, Delete ends a terminal or
> a process (`remote-end-process`, `proc/end`) after a confirm. `server/status`
> lists processes and per-workspace counts (no hard-coded zero left) and takes
> `{activity: true}`; `microide-server status` prints a table (`--json` the
> reply). See `openspec/changes/archive/2026-10-27-remote-host-activity`.
>
> **A Remote menu (2026-10-27, remote-menu).** Every remote verb that needs no
> typed argument is in a top-level Remote menu between Git and Terminal, labelled
> without the palette's `Remote:` prefix and dimmed (never hidden) while there is
> nothing to act on; File lost its Open Folder on Host twin. The host's status
> segment opens that menu above itself instead of running `remote-status`. Run
> from the menu, Stop Forwarding a Port… and Remove Mirror… pick from what exists
> (`remote-remove-mirror` now takes its root) and the host verbs ask which host
> when several are open. See `openspec/changes/archive/2026-10-27-remote-menu`.
>
> **Remote data safety under fault injection (2026-11-02, TD-2026-11-02-430).**
> `MirrorFaultMatrix` kills the client, kills the host's daemon or drops the link at
> every wire frame of a push, a journal replay, a pull and a conflict resolution,
> over a direct and a 60 ms link, and checks both trees hold exactly the expected
> files afterwards; `remote.py` Z does the same on the real app over ssh. It found a
> request completed twice when the link died mid-send (`RemotePeer`), write
> temporaries left as files by a killed writer (the server now never lists them; the
> mirror stages in `meta/staging`), and a host terminal left "detached" after a
> server restart. A file dropped onto a host terminal is uploaded
> (`file/upload`), and OSC 22 pointer shapes reach host terminals in a frame's flag
> bits.
>
> **Conflicts in bulk, and no false ones (2026-10-29).** Files the host changed
> overnight came up as Keep Local / Take Remote conflicts the user never made: a
> watched pull's base was saved only if something later happened to save, and a
> quit (`quick_exit`) never did (TD-2026-10-29-420). The pipeline now saves once
> its pulls drain, `Shutdown` persists every mirror, and an unflushed
> `meta/pulled` hint records each pull batch until the lazy state save does, so a
> crash cannot read a pull back as an edit either. From two conflicts on there is
> ONE counted row -- Keep All Local / Take All Remote / Manage… -- and Manage…
> opens a Conflicts sidebar view (`remote-conflicts`; Enter ticks a file; Select
> all / Keep Local / Take Remote / Merge act on the ticked ones;
> `remote-resolve-all [root] <local|remote>`). A parked conflict whose sides come to
> hold the same bytes settles itself. Design § 7.7, § 7.9b; QA `remote.py` Q.
>
> **`Remote: Open Terminal on Host…` removed (2026-10-27).** A remote project's
> Terminal > New runs on its host; the terminal-only host session behind the
> command (`RemoteHostService::Host`, its own status machine and auth path) is
> gone, so every host verb resolves a project's session. `Remote: Open Local
> Terminal` (Terminal > New Local Terminal) gives a shell on this machine from a
> remote-only window. Fixed on the way: `remote.term_credit_bytes` and
> `remote.scrollback_prefetch_lines` never reached a remote project's terminals,
> and a pane with its own local launcher skipped `terminal.shell`. See
> `openspec/changes/archive/2026-10-27-remove-host-terminal-command`.
>
> **Verified on a real host (2026-10-16).** `tools/qa/remote.py` drives the shipped
> binary headlessly against `worky` through the control channel -- open and first
> sync, save, host edits under clean and dirty tabs, offline save and replay, a
> killed ssh master, a host terminal, clangd on the host, a branch switch, eight
> agents churning while saving, a relaunch on the mirror -- and every scenario
> passes. Fixed on the way: a kept language server leaked on the host per client
> session (keep grace, `--keep-grace-ms`), the changed-on-disk banner was
> mouse-only (`banner-action`), `plugins-reload` reported the previous reload, no
> way to remove a mirror (`remote-remove-mirror`), no remote state on the control
> channel (`status.remote`), and the host entry points were palette-only (File
> and Terminal menus, the welcome page). The contract is now a spec:
> `openspec/specs/remote-project-mirror` (change archived 2026-10-25 as
> `openspec/changes/archive/2026-10-25-remote-project-mirror`). Left: TD-2026-10-16-344 (the cpp-lsp
> plugin's compile-database check reads the mirror, not the host).
>
> **Phase 3 done (2026-10-15).** Tuned on a real host (worky) under simulated
> agents: a git status loop on idle hosts, branch switches held as mass deletes
> (and a held-delete prompt with no Apply button), a 50 ms exit poll on every host
> process, and a full-window repaint per sync tick were found and fixed. Numbers
> and what was decided not to build (the resync threshold, the read-only git
> pool, the pty-handing restart) are in `dev-docs/design/remote-projects.md`
> § "Phase 3 against a multi-agent workload". Left: the perf scenarios stay
> advisory until `perf-runner-v1` (TD-2026-10-13-334), and the mid-scrollback gap
> fill (TD-2026-10-08-326). Phase 4 waits on a tree too big to mirror.
>
> **Phase 3, first slices (2026-10-12).** Submodule content sets, Changed on Host
> (§ 7.9), port forwarding with host-terminal localhost links, the ssh-config host
> picker, polling of tracked files under pruned directories, pull-on-open for stale
> and absent files, and the status segment's progress — see
> `dev-docs/design/remote-projects.md` § "Phase 3, first slices". Its
> follow-ups (the tree and quick-open from the manifest during a first sync, the
> project-home status block, a three-way conflict merge, per-host and automatic
> port forwarding) shipped 2026-10-12; TD-2026-10-12-336 is closed.
>
> **Phase 2b (2026-10-08): a remote PROJECT opens.** `Remote: Open Folder on Host…`
> works end to end on a mirror — see `dev-docs/design/remote-projects.md`
> § "Phase 2b as built" and `openspec/changes/archive/2026-10-13-remote-projects-phase-2b/tasks.md`
> (complete and archived 2026-10-13; its remainders, TD-2026-10-08-328, are resolved).
>
> **Phase 2a (2026-10-08): host terminals and the server.** Complete and archived
> 2026-10-14 (`openspec/changes/archive/2026-10-14-remote-server-phase-2a/`; its
> requirements now live in `openspec/specs/remote-*`, `notification-actions`,
> `save-continuations`, `performance-budgets` and `workspace-architecture`). The
> latency budgets are measured by `tests/perf/RemotePerfScenarios.cpp` over
> `tests/support/DelayRelay.h` (opt-in, advisory until `perf-runner-v1` baselines).
> What is left on the remote side: per-architecture static server packaging
> (TD-2026-10-08-325), filling a mid-scrollback gap when it scrolls into view
> (TD-2026-10-08-326, needs the host-mirror state moved out of `TerminalSession`),
> and the Phase 3 items that needed a real workload (done 2026-10-15, above).
> Phase 2a's build order, kept for the record: **(1) kernel test binary, (2)
> notification actions, (3) save continuations, (4) frame codec + transport, (5)
> daemon skeleton, (6) `proc/spawn` + `RemoteProcessLauncher` as the parity suite's
> third locality, (7) host terminals with prediction, (8) connection lifecycle and
> install, (9) budgets and lanes.**
>
> Decisions already made with the user — do not re-ask:
> - Notification actions are **VS Code-style inline buttons**; each is an
>   `ActionId` + args, never a closure.
> - Quit and close-project **wait for formatter saves with a progress row and a
>   Cancel button**; never freeze, never exit with a save in flight, never exit
>   unformatted on a timeout.
> - Everything in the design's § 11 table (server per uid, socket under `$HOME`,
>   `proc/spawn` with argv arrays, one ssh channel per project, mosh-style
>   prediction, no UDP, self-install, Phase 1 removed).
>
> Seams to build on, not around: `platform::ProcessLauncher` (Run owns cwd
> mapping; `LocalPathFromHost`; `ExpandWorkspaceFolder`), `project::ProjectLocality`
> (fixed at `OpenProjectTab`), `project::GitMetadataFor`, `HostPathTranslator`,
> `FileReadService` with `read_path = false` for off-thread waits, and
> `tests/parity/` (loopback launcher, recording local launcher, spawn-count check,
> shrink-only known gaps).
>
> Pitfalls this codebase has already paid for:
> - Decide a real-tool test's SKIP from a **local reference run**, never from the
>   run under test; the gdb parity row first passed with path translation off.
> - Never capture `this` of a `Make*Coordinator()` value in an async completion;
>   rebuild the coordinator in the completion.
> - Never edit source while `tools/run-checks.sh` is building a sanitizer lane.
> - `ctest | tail` hides the exit status; check `$?` of ctest itself.
> - Run the sanitizer lanes once at the END of a batch, with
>   `MICROIDE_BUILD_JOBS=6` (12 jobs ran out of memory on the ASAN build).
> - No time-based assertions while the machine is under load; parity budgets are
>   counts.

`dev-docs/design/remote-projects.md`. **Groundwork started on 2026-09-22; no remote
code exists.** The design measures what a remote operation costs (4,957 path
syscalls to open a project, 162 to open one file), rules out sshfs, and keeps a
local mirror equal to the remote tree through a persistent `microide-agent` daemon
over ssh, with every process (git, LSP, DAP, terminal) running on the host. The wire
is length-prefixed binary frames over a content-addressed object store with
`zstd --patch-from` deltas; the terminal model runs on the host and ships screen
state, with input as semantic events encoded there and mosh-style predicted echo
locally; one `microide-server` daemon per user outlives the connection, every host
process (git, LSP, DAP, tasks, shells) is its child, so terminals, builds and their
scrollback survive a dropped link and reattach; the server installs itself under
the user's home over the same ssh connection and needs nothing from the host but a
stock `sshd` login (revised 2026-10-07 after two design reviews; § 11 of the design
records each decision). No FUSE mount at any phase. Local and remote project tabs
coexist. Phase 1 was folded into Phase 2 on 2026-10-07; Phase 2a — the server,
surviving remote terminals, reattach — is the first user-visible slice and opens no
remote project.

**Groundwork is eleven local-tree changes, each of which stands on its own if remote
projects are never built.** Four are fully shipped (the design's own table at the top of
that file says exactly what, and what each one left):

- **G1 — the SDL-free kernel.** `microide_kernel` compiles util, platform, project,
  compare, persistence and terminal with a precompiled header that does not include
  SDL, so a kernel TU naming `Uint32` no longer compiles by accident.
  `microide_kernel_link_probe` links and runs every kernel object against libc++
  and pcre2 alone — 2.4 MB against the shell's 15.8. The wake path, the terminal's
  colour and modifier types, operational logging, the ANSI palette and path
  resolution all lost their SDL dependency; four separate dedups fell out.
- **G2 — one process launcher.** Every spawn routes through
  `platform::ProcessLauncher`, both primitives are linted, and `GitRepository` has
  no default launcher so all 25 sites state their locality. Git is now testable
  against a scripted launcher with no `git` binary. As of 2026-10-06 a PROJECT owns its launcher and write
  gate (`ProjectWorkspaceState::launcher()` / `write_gate()`): git (read and write
  side, blame), the LSP and DAP, the formatter, terminals, plugin tools and plugin
  writes all take their locality from it, `TextViewport::Save` takes the gate per
  call, and tree operations (create/rename/delete/trash) go through the gate's one
  transactional journal (TD-2026-09-22-301, -305, -308, TD-2026-10-06-318).
- **G3 — argv-shaped terminal launch**, which fixed `terminal.shell = "ssh host"`
  (ran `ssh -i`) and `terminal.shell = "bash"` (never resolved through PATH).
- **G9 (part)** — one `ResolveGitDirectory` instead of two that disagreed, then
  one `ReadFirstLineOfGitFile` instead of three readers of `.git` metadata files
  (each had a hole the others did not: two read the whole file to see its first
  line, the third read one line with no size cap). And git availability is now
  three answers — `Repository`, `NotARepository`, `Unknown` — with the status bar
  rendering a pending answer as `scm-unknown` rather than claiming `no-scm`. The
  local source never returns `Unknown`; the remote one is unknown until the first
  `git/metadata` lands, and that is the state a bool made structurally invisible.
- **G5 (part) — the format-on-save subprocess is off the shell thread.**
  `editor.format_on_save` defaults on and the bundled prettier plugin registers a
  formatter for every web filetype, so every Ctrl+S in a JS/TS project froze the
  window for as long as node took to start. An interactive save now defers: it
  returns without writing and its completion applies the formatter's output,
  guarded on the buffer's content revision so an edit made during formatting is
  never silently undone. Saves whose caller acts on completion still block.

- **G10 (part) — one gate for every content write into a project tree.** Six
  subsystems knew how to replace a file under the project root and shared the
  atomic-write primitive and nothing above it, so each grew its own post-write
  fixup. `project::FileWriteGate` is that place, it is where `MirrorWriteGate`
  plugs in, and the editor's save no longer stats a path it just wrote.

- **G6 (part, extended 2026-09-29) — a touch is not an edit, and confirming it
  is not a stall.** The watcher's echo check reads the file to tell a `touch`
  from a real edit, and that read ran inline: up to 8 MiB on the shell thread,
  per changed path, per watcher event — a `git checkout` that moves the mtime of
  every open file paid it for each of them with the window frozen. The check is
  three-valued now, the one case a digest decides is posted to G4's reader, and
  the sweep leaves that path undecided until it lands. The bytes stop on the
  worker; eight bytes cross the thread boundary, not eight megabytes. It also
  found that an external change to a clean buffer was reading the file three
  times on the shell thread — the sweep's check, the reload's own guard, and the
  reload itself — and removed the middle one. `FileSignature` carries a hash of the
  bytes it was recorded from, and a stat mismatch is confirmed against it before
  being believed. One method serves the save path and the watcher's echo check.
  This fixed a real, common false alarm: a `touch`, a `git checkout` that restored
  byte-identical content, or an agent rewriting a file with the same bytes all
  refused the save and announced a reload of a file nobody had changed.

- **G7 (part) — notifications have identity, lifetime and progress.** A row can
  carry a `key` (posting it again REPLACES the one on screen in place, so a
  changing count is one row rather than a stack of them), can be `sticky` (it
  reports a state and lives until its owner dismisses it by key, rather than
  vanishing four seconds after the state began), and can carry a `progress`
  fraction drawn as a bar. Sticky rows sit outside the transient visible cap and
  under a cap of their own. First caller: "Project too large to index fully",
  which is a state and was a four-second toast — after it faded nothing explained
  why search results were incomplete. Still open in G7: per-row ACTIONS
  (Reconnect, Compare, Show, Copy install command), which need hit-testing and
  action dispatch in the toast.

- **G4 (part) — a large file opens without freezing the window.** The read ran
  synchronously on the shell thread, so a generated blob, a cold disk or a slow
  device froze the window for its whole duration. Files at or above 4 MiB now
  read through `project::FileReadService` — one dedicated thread, not the serial
  executor a half-gigabyte read would park git status and search behind, with
  cancellation checked between chunks so closing the tab stops the I/O rather
  than finishing a read for nobody. The classification (content hash, encoding
  sniff, line-ending scan, CRLF rewrite — four more full passes) runs on that
  thread too. Below the threshold nothing changed: the read is a fraction of a
  frame there and a handoff would cost more than it saves.

  It is the WHOLE load, not just the read: the classification and the buffer
  build both run there, and a complete `TextViewport` is constructed on the
  worker and moved into the tab. One file is also one read however many panes
  wait for it — the completion hands its view to every other tab waiting on that
  path, sharing the buffer as the synchronous open always did.

  Four things fell out of it that stand on their own. `editor::AsyncBufferWork`
  is the guarded completion the design asks for once instead of per-consumer, and
  format-on-save moved onto it. `EditorTabState::needs_restore` became
  `Content{Ready, Deferred, Loading, Failed}`, so the ~40 guards that ask "is the
  viewport the file yet" cover a new state by construction rather than by
  remembering to widen forty conditions. `TextViewport` gained a read-only mode —
  because a tab that exists before its content does can be typed into, and that
  keystroke would become a buffer shadowing the real file that the next Ctrl+S
  writes over it. And writing the tests found a real bug in the split path: a
  pane cloned while its file was loading was left empty and read-only forever.

**Shipped 2026-10-07, all remote-shaped:**

- **G11 parity harness** (`tests/parity/`): the same scenario runs against a local
  project and a loopback non-local one with split host and mirror trees, compared
  on what the user sees AND on how many processes each started (a remote may not
  take more round trips than local). Rows: save, file operations, format-on-save,
  plugin tools, git sidebar, terminal, language server, plus real gdb and clangd
  through the loopback host. The known-gaps list may only shrink, and is empty.
- **Path translation** for language servers and debug adapters at the shared
  transport (`workspace/HostPathTranslator`), keyed by field name, inactive for a
  local project; `${workspaceFolder}` in server, adapter and plugin-tool argv.
- **Locality is identity**: `project::ProjectLocality` (launcher + write gate) is
  fixed at `OpenProjectTab` and survives the project-state reset.
- **Git asks the host**: `project::GitMetadataSource` via `GitMetadataFor(launcher)`
  (TD-319); git passes its root as the working directory, never `-C`; the outgoing
  base resolves in two processes instead of up to six.
- **Off the shell thread**: the conflict merge's three stage reads (TD-312), the
  review commands' git work (TD-323), and a rename/delete's formatter wait (part
  of TD-304).
- **Completion**: the list follows typing and refilters locally, re-asking only an
  incomplete server (TD-320), and opens itself on trigger characters and quick
  suggestions, once per word (TD-321).
- **Bugs found on the way**: the git sidebar could stick "refreshing" forever after
  a refresh during a refresh; a sandboxed plugin could not run git.

**G8** `ProjectId` shipped 2026-10-26 (`openspec/specs/project-identity`): a
project is named by `project::ProjectId` (`Local{path} | Remote{host, host_root}`),
fixed when its catalog entry is created and kept on `ProjectWorkspaceState::id`;
`root` stays the I/O path. Every label (project tab, badge, tooltip, window title,
prompts, the project home, the welcome recents, the control channel's `projects`)
derives from the id, and `ProjectLabelForRoot` is gone, so a remote project's
mirror path has nowhere to be shown. The session and MRU persist ids (dual-writing
the legacy root for one release), a restored remote project is remote before it
is activated, and a remote recents entry outlives its mirror. Nothing in
groundwork is open. Everything else listed here earlier has shipped: G4's size threshold (TD-2026-09-29-313, by the mirror), G5's
blocking callers (TD-2026-09-28-304), G6's blake3 manifest digest
(`util/ContentHash`), G7's notification actions (`notification-actions` spec),
and G11's parity harness. Groundwork carries its own perf gates (design § 9) and
coverage (§ 10).

Nothing here is remote code, and nothing here depends on remote projects shipping.
A change that only prepares for remote must not cost anything locally, so each
carries its own gates and coverage.

