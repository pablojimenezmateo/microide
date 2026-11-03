# MicroIDE Known Tech Debt

The queue of debt that is open, actionable and still present in the tree. Closed
debt does not live here: when an entry is resolved, decided or won't-do, it moves
to the dated ledger under `guidelines/tech-debt/archive/` (see "Where the history
lives" at the end), so the open work is not buried among the record of the closed.

Reviewed 2026-11-03: the file had drifted back to a record -- 290 of its 294 sections
were resolved, closed or decided, and the head of the file was ~500 lines of pass
summaries. They moved, verbatim, to
`guidelines/tech-debt/archive/2026-11-03-resolved-debt-ledger.md`; the four open
entries and the guardrails stayed.

## Open items

### TD-2026-11-03-431 — the 2026-11-03 subsystem bug hunt: what it found and fixed, and what it left. [OPEN — items 1, 2 and 5; 3 and 4 resolved 2026-11-03]

Every subsystem was read for defects, the 17 fuzz targets ran 15 minutes each (no
crash), and the QA monkey ran 30 fresh seeds across its three profiles (default
301-318 at 400 steps, editor and commands 41-46; all clean, redraw verifier on).
Fixed, each with a failing-first test:

- **A language server lost a snippet's mirror edits.** Typing into a placeholder
  edits each mirror with one `ReplaceRange`; the keystroke's LSP sync sent the last
  applied edit as the incremental delta, so a utf-8 incremental server (clangd)
  held `x Q=ab+ab` for a buffer reading `x Q=Q+Q`. The edit record now carries the
  revisions it spans and `TextViewport::AppliedEditFromRevision(rev)` hands it out
  only as the whole step from `rev`; the sync sends a delta only then
  (`Phase5.IncrementalSyncCarriesEverySnippetMirrorEdit`). The find-in-selection
  scope had the same shape and uses the same check.
- **"Conflict markers remain" matched substrings.** A resolved file with a `// ===`
  banner and a marker quoted in a comment or fixture read as unresolved forever,
  while the merge parser read it as no conflict. One line-anchored grammar
  (`util::IsConflict*Line`) serves the scanner, the text test, the staged-diff
  check and `ParseGitConflictOutput`.
- **A closed stdin at launch reached the first spawned program.** A pipe that took
  fd 0 was a no-op `dup2` in the child, which then closed it: an LSP server started
  with no stdin. `util::EnsureStandardDescriptorsOpen()` runs first in both mains.
- **fontconfig was asked again per raster** past 4096 distinct code points (the
  coverage memo was a capped set); it is a bitset over all scalars, and the asks
  are counted (`render.font_fallback_queries`).
- **The `bookmark` gutter icon's notch was inverted** (a blank row cut the pennant).
- **`RemoteHostService/APortThatStartsListeningIsForwarded` failed when the
  sanitizer lanes ran it at the same time**: each run probed a port, closed it and
  had a child bind it, all scanning from 20000. The ports are held now (the child
  binds and reports its own; bind-before-listen; a pid-derived base).
- The server build's three GCC warnings; and this file, split back to the open
  queue (290 closed sections to `guidelines/tech-debt/archive/2026-11-03-resolved-debt-ledger.md`).

Left open:

1. **Two more consumers of the last-edit record read it as the whole step.**
   Breakpoint shifting (`DebugService::ShiftBreakpointsForAppliedEdit`) and the
   stored diagnostics' shift (`LspService::ShiftLspDiagnosticsForAppliedEdit`) move
   by the last of several edits. Both only matter when an earlier edit of the
   step changed the line count (a mirror's edits do not), and diagnostics are
   republished by the server; neither keeps the revision it last followed, which
   `AppliedEditFromRevision` needs. The keystroke redraw is sized from the same
   record and relies on frame prep to repaint a snippet's other mirrors
   (`QaFlowRegression/TypingIntoAMirroredPlaceholderRepaintsEveryMirrorsLine` keeps
   that true).
2. **A save breaks a hard link.** The atomic write renames a new inode over the
   path, so a file with a second name keeps the old content under it (vim's
   `backupcopy=auto` writes in place when `nlink > 1`). Symlinks are resolved and
   kept; hard links are not detected at all.
3. ~~**`ProcessTable::ResolveExecutable`** resolved a relative `PATH` entry against
   the server's directory~~ **Resolved 2026-11-03**: against the spawned program's
   absolute `cwd`, as a shell there would (`RemoteProcess/ARelativePathEntryIsTheChildsDirectory`).
4. ~~**A dropped file whose name is over 200 bytes** was refused as "needs a file
   name"~~ **Resolved 2026-11-03**: any name up to NAME_MAX is taken, and a longer
   one is refused for its length (`RemoteServer/UploadPutsAFileOnTheHostPrivately`).
5. **qa-remote's 60 ms-link pass failed five cells under a fully loaded machine**
   (Z.resolve.link.0ms/150ms with one conflict left after 120 s, G.host-terminal,
   O.tui-starts, H.host-created-file-reaches-mirror); see the run on HEAD below for
   whether they hold on a quiet one.

### TD-2026-11-02-430 — remote data safety under fault injection, and the open-TD sweep after it. [OPEN — item 1 below]

The 2026-11-02 pass had only *probed* the remote sync engine. This one stressed it:
`MirrorFaultMatrix` (tests/MirrorFaultMatrixTests.cpp, 66 tests) runs a push (edit,
new file, large file, rename, delete), an offline journal's replay, a pull (by sync,
a large streamed one, and one a watch delta announces) and both conflict
resolutions against a real daemon (`microide-server start`), and kills the client,
kills the server or drops the link at EVERY wire frame the operation exchanges --
before the frame arrives and 0 µs, 300 µs and 2 ms after -- over a direct link and
a 60 ms one (`tests/support/FaultRelay.h` counts frames and fires at a boundary;
the client's crash is a real SIGKILL of this binary re-executed as
`--mirror-fault-client`). Every cell must recover to no error, nothing unsent or in
conflict, no conflict at all for a one-sided operation, and both trees exactly the
expected files. `tools/qa/remote.py` Z is the same end to end on the real app over
real ssh (app SIGKILLed and relaunched, host server SIGKILLed, ssh master killed),
in qa-remote's loopback and 60 ms passes: 36/36.

Found and fixed, each with a failing-first test:

- **A request the link's close answered was completed twice.** `RemotePeer` returned
  0 from a send that failed after the close had already failed its handler; every
  caller completed it again. The sync engine's outstanding-work count went below
  zero, `Flush()` returned with work running, and a mirror recovered after a link
  death mid-pull kept reporting "not connected" with the host's changes unpulled.
- **A write a crash interrupted left its temporary as a file.** A server killed
  mid-push left `.name.microide-<pid>-<n>.tmp` in the project, which the manifest
  listed and every mirror pulled; a client killed mid-pull left a partial copy in
  `tree/`. The server's content set never lists a write temporary and removes one
  whose writer is gone; the mirror stages its writes in `meta/staging`.
- **A host terminal whose server restarted stayed "detached" for good** (found by Z):
  the reattach's refusal was swallowed. It ends now, says why, and claims no handle.

Measured and found sound: every other cell of the matrix -- a push the host applied
without the answer arriving, a resolution interrupted at every frame, a pull the
client crashed while recording, journal replay -- recovers with nothing lost on
either side.

The sweep of every open TD after it, one commit each with a test (see each entry):
Save As's check-then-write window (429.6), the host's ephemeral port range (429.4),
quit waiting for a host terminal's open (428.6), a background stage staying with its
project (427.1), a crash snapshot's content hash crossing a restart (429.2), test temp
trees reaped and `$TMPDIR` checked (429.8), the background project tab's terminal
mark (428.3), the edge-triggered maximized panel (427.3), the debug pane filling the
band (427.4), a file dropped onto a host terminal uploaded (428.5), OSC 22 (428.1),
the hybrid port source (429.5), fontconfig per-glyph fallback (427.5), and a stale
paint QA seed 60 found at HEAD (a language change repaints the pane).

Left open:

1. **The matrix's steps are wire frames.** A client crash between two disk writes
   with no frame between them (the tree file and the journal of one save) is reached
   only through the after-delivery timings (0/300/2000 µs), not exhaustively; a
   power cut (lost unsynced writes) is not simulated at all. A link that dies
   without a FIN (a NAT timeout) is the heartbeat's to detect and is tested there,
   not in the matrix.

### TD-2026-11-02-429 — the error-management and data-safety pass: what it fixed and what it left. [OPEN — item 3; the rest resolved 2026-11-02]

The 2026-11-02 pass probed the shipped binary (control channel, SIGKILL / SIGTERM /
SIGHUP, chmod, deleted and moved roots, a crash between an edit and the file
changing) and the remote mirror (a crash before the journal, Remove Mirror), and
fixed, each with a failing-first test:

- **Data loss: a crash-restored buffer overwrote a change made while down.** The
  hot-exit snapshot was restored with no disk baseline, which `DetectDiskConflict`
  reads as "no conflict": Ctrl+S wrote the backup over the other edit, silently.
  The snapshot records its baseline (`PersistedEditorTabState::DiskBaseline`,
  `EditorTabTag` 35) and the restore rebinds it.
- **Data loss: steady typing never got a backup.** The crash-safety flush was a
  pure 2.5 s debounce every key reset; it is now bounded at 10 s after the first
  unstaged edit (`CrashSafetyFlushSchedule`).
- **Data loss: a power cut could revert a saved file.** No atomic replace fsynced
  its directory; the editor's save, persisted records and the host's synced tree
  writes now do (`util::FsyncDirectory`, ~0.44 ms per save measured on ext4).
- **Data loss: Remove Mirror deleted unjournaled work.** An edit in `tree/` nothing
  journaled, a file the state never heard of and an ignored file that exists only
  in the mirror counted as nothing; an unreadable state counted as empty
  (`MirrorStore::LocalWorkNotOnHost`).
- **Signals.** SIGTERM was ignored while the unsaved-changes prompt was open (a
  logout then SIGKILLed the app); SIGHUP killed it with nothing saved. One
  `TerminationSignalWatcher` turns all three into a forced quit with hot exit.
- **Remote terminals.** Unclaimed host terminals (a crash, another machine that
  detached) are reopened on connect instead of behind an Attach toast; a host
  terminal arms the crash-safety flush when its handle arrives.
- **Port-forward noise.** Only ports a project's program opened
  (`remote.auto_forward_ports_source`), never the ephemeral range, not announced
  twice in 5 minutes, and `remote.auto_forward_ports_notify` to forward silently.
- **Errors that said nothing.** A failed save names the cause (errno through the
  whole write chain, `util::DescribeWriteError`); replace-all names the first file
  it could not write; `project-open` says "no such folder" / "a file, not a
  folder"; an open project whose folder is deleted or moved says so.
- **Validation.** A killed QA driver's `/tmp/mqa-*` tree is swept by the next
  launch (37 leaked trees, ~2 GB, had filled the user's tmpfs quota: every test
  then failed with EDQUOT). A crash-before-journal test pins the sync engine's
  stat/ctime detection.

Checked and found sound: 40 saves interleaved with typing, 60 project switches in
a second with dirty buffers, close-project and quit with unsaved edits (hot exit),
Save As onto an existing file (refused), tree renames (`RENAME_NOREPLACE`), tree
deletes (trash), the read-only-file banner.

Left open:

1. ~~The root-missing row can go stale.~~ **Resolved in the same pass:** the
   window's focus gain re-checks while the row is up, and a folder that is back
   clears it and restarts the watcher (which re-indexes). Left: a folder put back
   while microide keeps the focus waits for the next focus change or switch.
2. ~~**A restored snapshot's baseline is stat-only.**~~ **Resolved 2026-11-02**
   (TD-430's sweep): the snapshot records the buffer's QuickContentHash beside a
   probe of the same function (`EditorTabTag::DiskBaselineContent`, 36); a restore
   trusts it only when the probe matches, so no new hashing (blake3 was not needed).
   Was: **A restored snapshot's baseline is stat-only.** The in-memory content hash is
   process-local (`std::hash`), so a `touch` while microide was down reads as a
   change and the save asks. Safe, but a stable digest (blake3, already linked)
   recorded at load would let it save.
3. **The crash-safety flush still snapshots buffers on the UI thread.** Measured
   (1-32 MiB dirty, 4 lanes loading the disk): the flush held the shell thread
   ~37 ms, ~28 of it ENCODING the session, ~1 ms the disk -- and with the new
   10 s bound it fired mid-typing. Fixed in the same pass: the session is encoded
   on the persistence worker (`PersistedRecordWriteQueue::QueueEncoded`) and the
   flush no longer waits; ~3.5 ms remain, copying the dirty buffers' lines
   (`lines().Snapshot()`), growing with their size. A shared piece-tree snapshot
   handed to the worker would remove that too. *Looked at 2026-11-02*: the piece
   tree owns `original_` and its add chunks by value, and the UI thread appends into
   the current chunk, so a shared snapshot means storage as shared immutable chunks
   (the full ones `shared_ptr<const>`, the open one copied or sealed at snapshot
   time) -- a redesign of PieceTree's storage, not a call-site change. The 3.5 ms at
   32 MiB is memcpy bandwidth: a cheaper copy does not exist. Left for that change.
4. ~~**The ephemeral range is Linux's default**~~ **Resolved 2026-11-02**: the server
   reports net.ipv4.ip_local_port_range in its hello (`ephemeral_ports`, advisory).
   Was: **The ephemeral range is Linux's default**, not the host's
   `ip_local_port_range`; the server could report the real one in `server/status`.
5. ~~**`project` port source misses daemonized programs**~~ **Resolved 2026-11-02**:
   a port nobody's process owns is the project's when its host terminal printed the
   address (the hybrid source, asked only when such a port appears).
   Was: **`project` port source misses daemonized programs** (a `docker run -p`, a
   service that double-forks): their socket's owner is outside microide's trees.
   Forward a Port… and `any` cover them; a terminal-output URL scan (VS Code's
   "hybrid") would too.
6. ~~**Save As has a check-then-write window**~~ **Resolved 2026-11-02**: a buffer
   whose baseline found no file writes with `WriteTarget::CreateNew`
   (RENAME_NOREPLACE) -- Save As and a new file's first save alike.
   Was: **Save As has a check-then-write window**: `exists()` then a rename that
   replaces. A RENAME_NOREPLACE for a Save As target would close it.
7. **Tests build failure text before the wait they report on**:
   `Expect(PumpUntil(...), "..." + ScreenText(x))` evaluates the message first
   (argument order), so a failure shows the state from BEFORE the wait.
   **Resolved in the same pass:** the 34 instances (all in remote and git tests)
   are lambdas, and `CheckWaitFailureMessagesAreLazy` (hard, with controls) keeps
   it that way.
8. ~~**The test binaries write their temp trees to /tmp**~~ **Resolved 2026-11-02**,
   measured first: a disk-backed TMPDIR made a kernel shard 26% slower and a long
   worktree path overflows the fake-ssh socket, so TMPDIR stays; test temp trees
   carry their owner pid and are reaped after a crash (91 removed on the first run),
   and run-checks.sh refuses to start with under 2 GiB free in $TMPDIR.
   Was: **The test binaries write their temp trees to /tmp**, a 31 GB tmpfs shared
   with every other tool; when another project filled the user's quota the whole
   suite failed with EDQUOT for reasons outside the product. `run-checks.sh`
   could point `TMPDIR` at `build/tmp` -- not done blind: every test's socket
   path grows with it, and the daemon tests already keep short directories
   (`mis.XXXXXX`, `mip.XXXXXX`) because of the 108-byte AF_UNIX limit; measure
   the longest socket path first.

### TD-2026-10-27-408 — the clang lane once read a header full of NUL bytes while six lanes built at once. [OPEN — not reproduced; the evidence is now kept]

2026-10-27: `tools/run-checks.sh clang-build` now keeps what this entry asked for
when a build fails with `null character ignored`: the failing pass's log, each
named header's hash and NUL count as it is on disk afterwards, the PCH files,
`ccache -s -v`, and `free` / `uptime` / the tail of `dmesg`, under
`$LOG_DIR/microide-clang-build-evidence/` -- and it still fails the lane (a retry
would hide it). Deferred until it happens again: nothing to fix without a
reproduction.

Original entry:

`tools/run-checks.sh clang-build`, run beside tests, ASan, UBSan, TSan and
hardened (2026-10-27), failed every TU that includes
`src/workspace/shell/WorkspaceShell.h` with `null character ignored
[-Wnull-character]` from line 18 on. The file on disk had no NUL byte and no
writer, and the same lane passed unchanged on the rerun right after. A read of a
source file that comes back zero-filled points below the build (ccache's or the
PCH's shared state under concurrent lanes, or the filesystem under memory
pressure), not at the tree. Not reproduced; next time keep the failing object's
`.d` file and the PCH beside the log, and check `ccache -s` for errors, before
deciding.

### TD-2026-10-27-406 — `RemoteTerminal/TwoClientsShareAHostTerminalAtTheSmallerSize` once saw a blank screen for 15 s. [OPEN — not reproduced; the evidence is now kept; 2026-11-02: 60 concurrent runs under load, all passed]

2026-10-28 correction: the "blank screen" was never observed. The failure
message was `Expect(WaitForText(a, ...), "..." + ScreenText(a))`, and C++ leaves
argument order unspecified: GCC built the message -- the screen, the handle and
the frame bytes -- BEFORE the 15 s wait ran, so it showed the screen at time
zero, blank by nature. What failed is only "the prompt did not arrive in 15 s".
The 2026-10-27 evidence below had the same flaw. Every such wait in
RemoteTerminalTests now builds its message after the wait (the lazy `Expect`
overload in tests/TestSupport.h), so the next failure shows the screen as it was.

2026-10-27: every test using `ShortServerDir` prints the last 32 KiB of its
server's log when it fails (the destructor sees the throwing `Expect` through
`std::uncaught_exceptions`), and this test's first wait names the terminal's
handle and the frame bytes the channel applied
(`RemoteTerminalChannel::received_bytes`) -- "no frame came" and "a frame came
and showed nothing" now read differently. Deferred until it fails again.

Original entry:

Under the ASan lane, while the GUI QA lane and the TSan build ran beside it
(2026-10-27), the first client's screen stayed entirely empty -- no prompt, not
even the echo of the typed `echo ready-$((1+1))` -- for the whole 15 s
`WaitForText`. Not reproduced since: the test alone 5x, 24 copies in parallel, and
12 parallel runs of its whole kernel shard, all under ASan, pass. A blank screen
(rather than a late one) is what a lost first frame would look like, and the
session-binds-before-first-request path (`TerminalSessionTestAccess::EnterHostMode`'s
early return) exists for exactly that race, so a starved server under four
concurrent lanes and a dropped frame are both possible. Next time it fails, keep
the server's log (`ShortServerDir`) and the channel's frame counters before
deciding which.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Preferring the Vulkan render driver for a faster launch** (2026-08-17). `SDL_CreateRenderer`
  costs 63-80 ms with SDL's own pick here (`opengles2`) and 41 ms with `vulkan`, and
  `SDL_HINT_RENDER_DRIVER` takes a comma-separated fallback list, so the ~30 ms looked free. It is
  paid back immediately: `render::TextBackend::UploadStringTexture` measures **8.9-10.6 ms per 95
  uploads on opengles2 and 27.1-29.0 ms on vulkan** — 3x, on the shell thread, on a path that runs
  on every glyph-cache miss. Trading interactive latency for a one-time 30 ms is the wrong direction
  for a project whose first priority is latency. Do not retry without re-measuring the upload path;
  the startup number is not the one that decides it. (`software` also takes ~75 ms to create here,
  which is the tell that the cost is the Mesa/EGL load rather than anything GL-specific — there is
  no cheap driver to switch to, only a different trade.)

- **Pre-warming the GL stack on a background thread** (2026-08-17). `SDL_CreateRenderer` is
  60-90 ms and traces almost entirely to Mesa's `eglInitialize` — a dlopen chain of 35 shared
  objects including libLLVM, plus DRM ioctls. The shell thread is idle before it, so warming the
  stack from a thread started at the top of `Application::Initialize` looked like most of that
  moved off the critical path. Measured, ten interleaved rounds each, time from process start to a
  created renderer: **108.8 ms cold vs 121.2 ms warmed** — worse. The warm does cut SDL's own
  renderer creation (86 -> 58-64 ms), but it costs 62-66 ms to do it and only ~20 ms of that
  overlaps anything (`SDL_Init` plus window creation), so the join eats the rest. Warming with
  `dlopen` alone and no `eglInitialize` costs 0.4 ms and saves nothing (99.5 vs 102.8 ms): the
  expensive part is the driver initialization, not loading libEGL. And an EGL display we initialize
  on the default (X11/DRM) native display is not the one SDL initializes on the Wayland one, so
  part of the work is done twice by construction. Do not retry without a way to warm *SDL's own*
  display before it needs it.

- **Deferring VSync until the window is mapped** (2026-08-17). The renderer is created with VSync on
  and the first two presents (the startup frame, and the re-blit after `SDL_ShowWindow`) happen
  while the window is still hidden, so pacing them to a refresh boundary looked like up to a frame
  interval each of pure waste. Measured five interleaved launches each on a Wayland session: time to
  `Application::FirstRender` **104.9 ms vs 105.9 ms** — no effect. A present against an unmapped
  surface does not block for a frame callback that was never requested, so there was nothing to
  save. Reverted rather than kept "because it cannot hurt": it is a second VSync state to reason
  about for zero measured gain.

- **Capacity-preserving line assembly in `PieceTree::ExtractLineRange`** (2026-08-10). The spanning
  line path does `out.push_back(std::move(current))`, which hands the buffer away and leaves `current`
  at zero capacity, so the next line that spans pieces re-allocates and grows geometrically. Copying
  out of `current` instead (and constructing single-piece lines straight into `out`) makes it exactly
  one allocation per line in every case. Measured **zero** on `editor_long_line_select_all_edit`,
  `editor_typing_minified_line`, `editor_smart_indent_typing`, `editor_sort_lines_large`,
  `first_line_edit`, `mid_file_edit`, `typing_large_file`, `toggle_line_comment` — before and after,
  same build, identical counts. The reason is structural: an insert appends its whole text as ONE
  piece, so a multi-line slice's lines do not span pieces; only an intra-line edit splits a line, and
  the paths that slice after those edits record inline undo entries instead. The shape the fix
  addresses is not produced by the editor. Do not retry without first exhibiting a scenario whose
  slice actually spans pieces.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.
- **`UNITY_BUILD` on `microide_tests`** (build-speed). Rejected 2026-06-29: 116 of 121 test TUs
  define helpers in anonymous namespaces, with confirmed same-name collisions across files
  (`MakeService`, `MakeViewport`, `MakeFixtureRoot`, …). CMake's unity batching concatenates files
  into one TU without isolating their unnamed namespaces, so those become colliding
  `(anonymous namespace)::` symbols → redefinition errors. Making it compile would mean rewriting
  ~116 files (per-file `UNITY_BUILD_UNIQUE_ID` namespace wrapping) for a speculative win the PCH pass
  below already captured the safe part of. Do not retry without first solving the anonymous-namespace
  collision mechanically. The shipped win instead was **precompiled headers** (stable std + SDL set,
  shared by `microide`/`microide_tests`/`microide_perf`): clean `microide_tests` build 141.3 s → 120.0 s
  (~15%), suite still green, runtime byte-for-byte unchanged.

## Shipped build-speed wins (2026-06-29, after the PCH pass)

- **Shared `microide_core` object library.** `MICROIDE_CORE_SOURCES` (≈373 TUs) is now an
  `OBJECT` library compiled once and spliced (`$<TARGET_OBJECTS:microide_core>`) into
  `microide`/`microide_tests`/`microide_perf`, instead of being re-listed in each target. This
  removed the previous double compile of the entire core. The prerequisite was making core
  **`MICROIDE_TESTING`-free**: ABI-neutral seams (`*ForTesting` methods, `TestAccess`/test friends,
  the `before_cache_apply_hook`, `test_sent_bytes_`) are now compiled unconditionally, and the
  genuinely behavioral forks (placeholder-vs-real terminal startup, project-init default terminal,
  `SendBytes` capture) are gated at **runtime** via `terminal::SetUsePlaceholderTerminalsForTesting`
  (the test/perf `main()` enables it), mirroring the existing `SetHostPlatformOverrideForTesting`
  precedent. Core must stay free of `#ifdef MICROIDE_TESTING`, or the shared object set diverges by
  ABI between binaries. Measured: clean build of `microide` + `microide_tests` 148 s → 91 s (~38%,
  ccache disabled for the comparison). `WorkspaceShellTestAccess.h` keeps its guard — it is a
  test-only header, never compiled into core.
- **ccache + ld.lld + split-dwarf**, all conditionally enabled in `CMakeLists.txt` (no-ops when the
  tool is absent; toggle with `-DMICROIDE_USE_CCACHE=OFF` / `-DMICROIDE_USE_LLD=OFF`). lld is skipped
  under LTO so the `microide-perf` preset keeps its default linker. `tools/run-checks.sh` exports
  `CCACHE_SLOPPINESS=pch_defines,time_macros` so PCH TUs cache.
- **Scoped inner-loop build.** `tools/run-checks.sh tests` and the documented loop build
  `--target microide_tests` (the only binary `ctest` invokes), skipping `microide` and the bench
  binaries.
- **Splitting `microide_tests` into multiple filtered `add_test` invocations for `ctest -j`**
  (test-run speed). Rejected 2026-06-29: partitioning ~121 files' worth of tests by name-substring
  filters is a silent-coverage hazard — a test matching no subset is dropped from the run with no
  signal, trading a correctness guarantee for ~2× wall-clock on an already-acceptable 47 s suite. The
  in-binary substring filter (`TestRunnerCli`) already covers focused local iteration. Do not retry
  without a mechanically-proven complete-and-disjoint partition (e.g. generated from the registry, not
  hand-maintained filter strings).

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, the
2026-06-15/16 render/app/util/terminal closeouts, and the 2026-07-12 deferred-backlog sweep (the
cross-subsystem bug-hunt passes 5–24 closeout) — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons),
  and the two ledgers of entries resolved here: `2026-08-04-resolved-debt-ledger.md` and
  `2026-11-03-resolved-debt-ledger.md` (every TD id from 2026-08-04 to 2026-11-02 not listed
  above, with the per-pass review summaries that headed this file)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
