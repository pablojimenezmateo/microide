# Resolved debt ledger — moved 2026-11-08

Moved verbatim from `dev-docs/project/known-tech-debt.md`'s open queue: every entry
below is resolved or closed. Newest first, as they stood.

### TD-2026-11-08-434 — the 2026-11-08 QA-monkey bug hunt: what it fixed, and the one seam it left. [CLOSED 2026-11-08 — item 1 not reproduced; guarded]

Baseline at 78a043e81: asan, ubsan, tsan and qa (228/228) clean. Then the QA monkey
on fresh seeds (default 601-620 at 400-500 steps, editor 71-74, commands 71-76) and
the editing invariants (plain and soft-wrapped, clean). Every deterministic finding
replayed, pinned by a failing-first test, and fixed, one commit each:

- **A menu closing over a floating widget left its hover unpainted** (seed 605). The
  frame-prep hover catch-all repaints what `HoveredInteractiveRect` names when it
  changes without motion; the find widgets and the debug toolbar were the last
  PointerOver buttons it did not know. They are elements of it now, and the motion
  handler's separate floating-widget collector (~50 lines) is gone
  (`QaFlowRegression/ClosingAMenuOverAFindToggleRepaintsItsHover`).
- **A focus move by the maximized-panel rule painted no focus ring** (seed 601).
  `SyncPanelMaximizedWithFocus` ran in frame prep after the text-input sync and the
  surface view models had read focus; it runs first now, and an assert at the
  focus-ring tracking pins the order
  (`QaFlowRegression/AFocusMoveByTheMaximizedPanelRulePaintsTheRingInItsFrame`).
- **TD-2026-10-30-426's open abort, root-caused** (seed 601, deterministic). Each
  view of a document keeps its own width and wrapped-row tables, and their
  incremental splices checked line counts but not the revision the table was built
  for: an edit through one pane of a split, then one through the other, spliced
  into a stale table and stamped it current. A release build laid a line out
  against its old width. Both splices now require the table to describe the text
  just before the edit (`TextLayoutCache::DescribesTheTextJustBefore`;
  `TextViewport/AnEditThroughTheOtherViewOfADocumentKeepsItsWidthTableTrue`; QA flow
  `split-wrap-edits`, which crashes the pre-fix build).
- **Typing under sticky-scroll headers repainted the wrong row** (seed 608):
  `CurrentEditorLineRangeRect` and the IME's text-input area laid rows out with no
  sticky band (`QaFlowRegression/TypingUnderAStickyBandRepaintsTheTypedRow`).
- **The commit body took clicks through a modal prompt, and kept a caret** (seed
  607): its hit-test ran ahead of the modal checks (`OverlayIsModal` is new), and
  the commit fields blinked by the global phase without owning the keyboard
  (`QaFlowRegression/ACommitFieldUnderAModalPromptTakesNoClickAndPaintsNoCaret`).

Left open:

1. **A 1-px seam at the split divider at a 1.75x UI scale** (seed 602, default
   profile, step 361; not reproduced on replay). In a partial frame the left pane's
   rightmost device column (x=924) kept the editor background where a full frame
   draws a 1-px line of colour (231,234,240) -- the pane's edge element, one device
   column left of the divider. A model of the clip narrowing (one and two nested
   levels, every aligned surface and damage rect up to 40 logical px at 1.25x and
   1.75x) finds no divergence, so the narrowing math is not it. Suspects: the pane
   clip in `RenderClip` is built as `{floor(x), floor(y), ceil(w), ceil(h)}` from a
   fractional pane rect (its right edge can fall up to a logical pixel short of
   `x + w`), and the timing (the replay passes). Evidence:
   `build/qa-evidence/seed-602-fractional-seam-2026-11-08`. Next step: a unit test
   at a 1.75x logical presentation with a split whose divider sits at a fractional
   logical x, painting a partial frame whose damage ends inside the left pane.
   *Closed 2026-11-08*: that test exists now --
   `QaFlowRegression/PartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit`,
   painting at 1.25x and 1.75x as Application does, in seed 602's state, across
   the divider and the sidebar's edge; it fails when partial frames are hinted with
   the integer clip (the defect DeviceClipHint fixed), so it is not vacuous. It
   finds no seam, and six parallel replays of seed 602's run were clean. Left
   unexplained: the one GL-rendered occurrence; the probe and the qa lanes'
   verifier keep watching.

Also fixed after the first five, from writing their tests: three more pointer paths
without the sticky band (right-click caret, gutter breakpoint menu, above-line lens
click; `RightClicksUnderAStickyBandActOnTheClickedLine`), and the control channel's
`ui` rows, which left out inset gaps (`VisibleRowTops`). TD-433's stall recurred in
this session's qa-wayland lane with its first thread dump; see there.

### TD-2026-11-07-433 — qa-wayland: the app stops answering the control channel after its first file open. [RESOLVED 2026-11-08 — SDL 3.4's lost cross-thread wake; app::LostWakeRelay]

Seen twice now, each time with several GUI lanes running side by side: TD-432's
`caret-survives-view-changes` (2026-11-05) and `editor-toggles` (2026-11-07,
wave 3 beside QA). Both are editor-only flows, and in both every control command
times out from the first `open` on, while the app log shows the startup frame
and nothing after it -- the main loop stopped servicing the channel (or never
woke for it) rather than one command failing. Both passed on their retry; 8
parallel drivers of `editor-toggles chrome menus` on weston passed. X11 has
never shown it. The driver now records, on its first control-reply timeout,
every thread of the stuck app with its name, state, kernel wait channel and
current syscall (`stall-threads.txt` beside the finding's screenshots; `/proc`,
since yama's ptrace scope 1 refuses a sibling gdb). The next occurrence says
whether the main thread sits in poll/futex (SDL_WaitEvent, a wayland roundtrip)
or is running our code. Evidence: `build/qa-evidence/wayland-editor-toggles-2026-11-07`.

*2026-11-08, third occurrence, now with the thread dump* (`plugin-editor-api` in a
qa-wayland lane beside qa-asan and six monkey drivers; passed on retry): the main
thread sat in `poll(1 fd, timeout -1)` -- SDL's `Wayland_WaitEventTimeout` blocked
on the display fd for READ -- while `control-io` was alive in its 1 s poll. The
command arrived; `SDL_PushEvent`'s cross-thread wake never woke the main loop. In
SDL 3.4.2 that wake is `Wayland_SendWakeupEvent`: `wl_display_sync` plus one
`wl_display_flush`, with no `EAGAIN` handling (the waiter's own flush retries on
writability; this one does not). Leading hypothesis: under load that flush hits
`EAGAIN`, the sync stays in the client buffer, and nothing flushes it while the main
thread polls for READ only -- lost until unrelated input, which a kiosk QA session
never sends. That would explain "Wayland only, only under parallel load".
Unconfirmed. Next step: an `LD_PRELOAD` shim logging off-main-thread
`wl_display_flush` `EAGAIN` returns during a stressed qa-wayland run; if confirmed,
report upstream and stop depending on a foreign-thread flush for the wake.
Evidence: `build/qa-evidence/wayland-plugin-editor-api-2026-11-08`.

*Resolved 2026-11-08* -- not the flush. SDL 3.4.2's `SDL_WaitEventTimeout_Device`
checks the queue, THEN publishes `wakeup_window` (an atomic; the lock older SDL
held across both is gone), then blocks, with no re-check: an `SDL_PushEvent` from
another thread in between finds no window, sends no wake, and the loop blocks
with the event queued. Reproduced by `tools/qa/stress/sdl_wake_race.cpp` (4 lost
wakes in 240k pushes under headless weston with 12 copies; none with the fix).
`app::LostWakeRelay` re-delivers a wake pushed into a blocking wait that is still
blocked 10 ms later, at no idle cost (38b167725). Worth reporting upstream; the
relay stays harmless if SDL fixes it.

### TD-2026-11-05-432 — the 2026-11-05 terminal UI/UX review: what it fixed, and what it left. [RESOLVED 2026-11-06 — 1 and 2]

A review of the terminal as a user meets it -- selecting, copying, the cursor --
driven through the QA harness's screenshots before and after. Fixed, each with a
failing-first test:

- **The selection was nearly invisible and erased the output's colours.** It
  painted `theme.row_highlight` (the hover/cursor-line tint) under
  `theme.text_primary`, and stopped at each row's last cell. Row paint is now a
  pure planner (`workspace/render/TerminalRowPaint`): each cell resolved once, the
  selection and find hits a wash composited over the cell's own background, its
  foreground kept at WCAG AA contrast, the line break of a row the selection runs
  past washed to the pane's edge, a wide glyph emphasized with its lead (what the
  copy keys on), an unfocused terminal's selection at half strength. Paint used to
  ask `WorkspaceShell::TerminalCellSelected` per cell -- a state walk ~5x per cell
  per frame from a render TU; the pane now carries `frame_selection`, sampled in
  frame prep, which repaints the pane when it changes.
- **A selection survived a program entering or leaving the alternate screen**,
  highlighting (and, on Ctrl+C, copying instead of interrupting) whatever vim drew
  over the rows it named. The row-space rebase drops it.
- **Drags after a double/triple click went cell by cell** (dragging back from a
  double-clicked word dropped the word) **and Shift+click did nothing.**
  `ExtendTerminalSelection` extends by the selection's unit from its origin.
- **The cursor ignored DECSCUSR**: parsed, never drawn -- always a blinking block.
- **No Select All**; and the copy snapshotted every selected row (~150 MB for the
  100k-line cap) before its 8 MB text cap applied. It streams in 256-row chunks.
- **The hovered link's underline was the theme's text colour**, not the link's.
- **Paint cost**, measured by the new `terminal_paint_colored_selection` scenario
  (A/B against the pass's base, interleaved, `WorkspaceShell::RenderBottomPanel`
  under `MICROIDE_PERF_SUMMARY`): a plain coloured screen 8.4 -> 6.3 us per paint;
  a wholly selected one ~14 us, after `RelativeLuminance` went to a lookup table
  (the selection's contrast correction was ~25 us of `pow()` per paint before).

Decided, not debt: SGR 5 (blink) is stored and not drawn, as in VS Code's
terminal by default; `CSI 4:3 m` (curly) draws as a plain underline.

Left:

1. ~~**Bold and italic are not drawn as faces.**~~ **Resolved 2026-11-06.**
   `FontRequest` carries a `FontStyle`; the SDL_ttf backend opens the family's
   own Bold / Italic / BoldItalic file -- sibling naming first (`X-Regular` ->
   `X-Bold`, `DejaVuSansMono` -> `-Oblique`), then fontconfig, accepted only for
   the same family at the asked weight and slant -- and synthesizes the style
   (`TTF_SetFontStyle`) when the family has none, as for the bundled JetBrains
   Mono Regular. `TextRenderer::Face(style)` makes a styled sibling on the same
   pool on first use and keeps it on the renderer's family and size, so a style
   nothing prints costs nothing. The row planner splits glyph runs on the face
   and records each run's cells; a face whose advance is not the grid's (a
   synthesized bold is wider) is placed cell by cell, so the grid never drifts.
   Tests: `TerminalRowPaint/TextRunsSplitOnTheFace`,
   `TextRenderer/FacesAreMadeOnFirstUseAndFollowTheFont`, the SDL_ttf styled-face
   test (sibling resolution, and bold drawing heavier); QA `terminal-escapes`.
   Was: **Bold and italic are not drawn as faces.** SGR 1 brightens a basic-palette
   foreground (as xterm's `boldColors`) but a truecolor or default-coloured bold
   cell looks like any other, and SGR 3 changes nothing. A face is a font: the
   fix is a style in `render::FontRequest` (resolved through fontconfig to the
   family's Bold/Italic file, synthetic `TTF_STYLE_BOLD` only as the fallback),
   a backend per style from the shared `TextBackendPool`, and the row planner
   splitting glyph runs on style as it does on colour. The ASCII atlas and the
   cluster cache are per backend already, so the cost is one more atlas per style
   in use, paid only by a terminal that prints that style.
2. ~~**Underline colour (SGR 58/59) and the styles past double (4:3..4:5)**~~
   **Resolved 2026-11-06.** The shape takes the style's two free attribute bits;
   the colour shares the cell's 16-bit id with the OSC 8 link
   (`TerminalCell::extra` -> `TerminalCellExtraTable`, interned `(URI, colour)`
   pairs, the 18-byte cell unchanged). A line snapshot carries the colours its
   underlined cells name, so paint never reads the table; the planner coalesces
   strokes by shape and both colours, and the pane draws curly, dotted and dashed
   lines. Remote: wire version 4 adds the colours as their own frame section,
   sent only to a client whose hello said 4 (the floor stays 3, so an upgrade
   restarts no host server). A host history page (term/scrollback) still drops
   extras, as it dropped links. Tests: `TerminalSession/KeepsUnderlineShapesAndColours`,
   `TerminalRowPaint/UnderlinesTakeTheirColourAndShape`,
   `TerminalHost/UnderlineColourReachesTheClient`, the wire round-trip's v3 peer.
   Was: **Underline colour (SGR 58/59) and the styles past double (4:3..4:5) are
   parsed and dropped.** A colour does not fit the 18-byte `TerminalCell`
   (`static_assert`); it wants out-of-line extended attributes keyed by cell, the
   way xterm.js stores them, which the cell comment already names for long
   grapheme clusters. Curly underlines are what language-server-aware TUIs
   (helix, neovim's terminal UI) use for diagnostics.

Seen in the pass's validation, not caused by it: qa-wayland's
`caret-survives-view-changes` (an editor-only flow) failed once with every
control command timing out from its first file open -- three GUI lanes ran side
by side -- and passed on its retry alone (flaky, the lane green). Not reproduced;
the evidence was `microide-qa-wayland-out/caret-survives-view-changes`.

### TD-2026-11-03-431 — the 2026-11-03 subsystem bug hunt: what it found and fixed, and what it left. [RESOLVED 2026-11-03 — 2 decided; 1, 3, 4 and 5 resolved]

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
- **`RemoteHostService/AOneSidedConflictOffersNoMerge` failed ~1 run in 55**: the
  "offline" save was made before `remote-disconnect` had taken the link down, so it
  was pushed and moved with the host's rename (no edit lost; the test never
  reached its conflict). `DisconnectAndWait` waits for the status bar's
  "disconnected"; 0 of 80 since.
- **perf-tests and perf-canary built one tree without its lock** and, side by side,
  clobbered a configure; both take `locked_build` now.
- **The caret misplaced (user report, 2026-11-03), three causes.** Go to File and
  project search drew their caret 8 px above the field: the five quick-open modals
  share one field row since e55931205 and the caret kept the old one; it reads the
  view model's row now, and `QaFlowRegressionUx/EveryFieldsCaretSitsOnItsText`
  checks ten fields with a pixel oracle. In a terminal running Claude Code the
  screen, caret included, was one keystroke behind: a PTY read holding one
  synchronized frame's close and the next one's open suppressed the wake for the
  finished frame. And frame prep read a terminal's cursor under a second lock from
  its lines, so a frame could pair them across an output.
- QA: `lsp-snippet-mirrors` (clangd's copy keeps up with a snippet's mirrors; it
  times out with the old sync check restored).
- The server build's three GCC warnings; and this file, split back to the open
  queue (290 closed sections to `guidelines/tech-debt/archive/2026-11-03-resolved-debt-ledger.md`).

Left open:

1. ~~**Two more consumers of the last-edit record read it as the whole step.**~~
   **Resolved 2026-11-03.** Breakpoints slid through the last edit on EVERY
   keystroke redraw, including redraws with no new edit (a Tab between snippet
   placeholders moved a breakpoint again per Tab); they now remember the revision
   they followed (`DebugService::ShiftBreakpointsForViewportEdit`). The stored
   diagnostics' shift takes the same rule, and neither slides through a record
   that is no longer the latest change (a reload does not clear it:
   `TextViewport::LatestAppliedEdit`). Left: the keystroke redraw is sized from the
   same record and relies on frame prep to repaint a snippet's other mirrors
   (`QaFlowRegression/TypingIntoAMirroredPlaceholderRepaintsEveryMirrorsLine`
   keeps that true).
2. ~~**A save breaks a hard link.**~~ **Decided, already**: the atomic write renames
   a new inode over the path, so a file's other names keep the old content. That is
   the documented trade-off for crash-atomic saves, pinned by
   `SaveDataIntegrity/AtomicSaveReplacesInodeAndBreaksHardlink` (an in-place write --
   vim's `backupcopy=auto` for `nlink > 1` -- can leave a partial file on a crash).
   Recorded so the next pass does not re-derive it; reversing it is the user's call.
3. ~~**`ProcessTable::ResolveExecutable`** resolved a relative `PATH` entry against
   the server's directory~~ **Resolved 2026-11-03**: against the spawned program's
   absolute `cwd`, as a shell there would (`RemoteProcess/ARelativePathEntryIsTheChildsDirectory`).
4. ~~**A dropped file whose name is over 200 bytes** was refused as "needs a file
   name"~~ **Resolved 2026-11-03**: any name up to NAME_MAX is taken, and a longer
   one is refused for its length (`RemoteServer/UploadPutsAFileOnTheHostPrivately`).
5. ~~**qa-remote's 60 ms-link pass failed five cells under a fully loaded machine**~~
   **Resolved 2026-11-03**: load, not the product. Run alone at 20cad2dbb, qa-remote
   passed all 262 cells, the slow-link Z, G, O and H cells among them.

### TD-2026-11-02-430 — remote data safety under fault injection, and the open-TD sweep after it. [RESOLVED 2026-11-06 — item 1]

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

1. ~~**The matrix's steps are wire frames.**~~ **Resolved 2026-11-06.** Every
   durable-write boundary of the client is a `util::CrashPoint` (a relaxed atomic
   load in production): `util/DurableFile`'s write / rename / directory sync, tree
   writes, `syncfs`, the pulled hint, the object store. A second matrix axis
   (`MirrorFaultMatrix/Disk/<scenario>/KillClient`) counts the points an
   undisturbed run passes and SIGKILLs the helper at each one; a third
   (`.../PowerCut`) also journals every point with the pre-image of each rename's
   source and target, and after the kill applies a power-cut model -- a rename no
   later sync of its directory (a deferred pull's: no later `syncfs`) covers is
   undone, unsynced appends and objects are torn -- before the usual recovery.
   It found, each now fixed:
   - **A save's temporary left in the mirror's tree for good** (crash between
     writing it and renaming it). Mirror saves stage it in `meta/staging`
     (`project::WriteTextLocally`), which `MirrorStore::Open` sweeps; a local
     project's scan and the watcher's walk remove one whose writer is gone and
     that is over 10 minutes old (`util::SweepAbandonedTemporaryStaging`).
   - **A new file or a rename that landed before its journal never reached the
     host**, and the mirror reported nothing unsent. The first sync after opening
     adopts files no entry records: as the rename it was when it holds exactly the
     base of the one entry whose file is gone, else as a new file to push
     (`AdoptUntrackedLocalFilesLocked`; paths a journaled op covers are left to it).
   - **A stale "not connected" after a reconnect** (seen once under load): a
     fetch the dead link failed posted its error after the new link's full sync
     cleared it. Fetch results carry the full-sync epoch they were issued in.
   Was: **The matrix's steps are wire frames.** A client crash between two disk writes
   with no frame between them (the tree file and the journal of one save) is reached
   only through the after-delivery timings (0/300/2000 µs), not exhaustively; a
   power cut (lost unsynced writes) is not simulated at all. A link that dies
   without a FIN (a NAT timeout) is the heartbeat's to detect and is tested there,
   not in the matrix.

### TD-2026-11-02-429 — the error-management and data-safety pass: what it fixed and what it left. [RESOLVED 2026-11-06 — item 3; the rest 2026-11-02]

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
3. ~~**The crash-safety flush still snapshots buffers on the UI thread.**~~
   **Resolved 2026-11-06.** The piece tree's storage is shareable: the original
   is a `shared_ptr<const std::string>` a reset or compaction replaces, and add
   chunks are fixed-capacity `shared_ptr<char[]>` arrays only ever written past
   their size (a copy of the tree, which shares them, starts a chunk of its own).
   `PieceTree::Snapshot()` is an O(pieces) walk returning an
   `editor::DocumentSnapshot` -- byte spans plus their owners -- readable on
   another thread while the tree is edited. The session record carries one
   (`PersistedEditorTabState::buffer`) and the encoder streams its lines on the
   persistence worker. Measured on a 24 MB, 400k-line buffer with ~4000 pieces:
   90 ms -> 0.42 ms on the shell thread per flush. Guarded by
   `CrashSafetyFlushPersistsUnsavedContent` (no whole-document copy during the
   flush, and typing after it does not reach the record),
   `PieceTree/SnapshotReadsAsTheDocumentDidWhenTaken`,
   `PieceTree/SnapshotIsReadableWhileTheTreeIsEdited` (TSan) and the
   equivalence fuzzer, which now holds every snapshot it took and a copy of the
   tree edited alongside.
   Was: **The crash-safety flush still snapshots buffers on the UI thread.** Measured
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

### TD-2026-10-27-408 — the clang lane once read a header full of NUL bytes while six lanes built at once. [CLOSED 2026-11-06 — not reproduced; the capture stays armed]

2026-11-06: closed as not reproducible. Since the evidence capture went in, the
clang lane has run in every validation wave, and on 2026-11-06 it ran again
beside six other lanes (tests, ASan, UBSan, TSan, QA, perf-tests at four jobs
each) -- the exact condition -- and was clean. Nothing in the tree can produce a
zero-filled read of an unchanged header; the capture in `run-checks.sh` still
fails the lane and keeps the failing pass's log, the headers' hashes and NUL
counts, the PCH files, `ccache -s -v` and the machine's memory and kernel log,
which is where a new entry would start.


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

### TD-2026-10-27-406 — `RemoteTerminal/TwoClientsShareAHostTerminalAtTheSmallerSize` once saw a blank screen for 15 s. [CLOSED 2026-11-06 — not reproduced in 252 runs; the evidence is kept]

2026-11-06: closed as not reproducible. 192 more runs under ASan, 24 at a time
(eight waves), all passed -- 252 with the 60 of 2026-11-02. The "blank screen"
was never observed (the message was built before the wait, see below), the
failure's message now shows the screen after the wait with the server's log and
the channel's frame counters, and a recurrence fails the test with exactly that
evidence.


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

