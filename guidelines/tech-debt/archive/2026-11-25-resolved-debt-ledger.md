# Resolved debt ledger — moved 2026-11-25

Moved from `dev-docs/project/known-tech-debt.md`'s open queue as each entry was
resolved or closed, with its resolution appended. Newest first.

### TD-2026-11-09-436 — soak triage, 2026-11-09: what the first four hours left open. [CLOSED 2026-11-25]

The soak runner's first four hours on `6d7e96e07` produced 113 signatures. The
2026-11-09 pass fixed the classes it could reproduce or prove with a failing-first
test (inverted std::clamp at ui.scale 3; overlapping status-bar segments; the find
widget, tab-strip contents, banner and merge-toolbar buttons leaving their area;
the crash-safety flush missing edits to a non-active buffer) and left these:

- **A tab drag abandoned with Escape, then released, paints stale: FIXED
  2026-11-25.** The motion of a tab (or path) drag never recorded the pointer,
  so after Escape the tooltip was laid out at the press point, and the release
  moved it without asking for a frame. Drags now record the pointer like every
  other gesture (`QaFlowRegression/ATabDragAbandonedWithEscapeThenReleasedPaintsFresh`,
  8953 stale pixels before).
- **Deterministic stale frames whose minimal replay still fails at `193dda043`:**
  `9e662fe598fe` (tab drag across three panes: the bare gesture is fresh, so a
  precondition of the 198-action minimal matters), `7f858b8312bb`, `b5c3ab96f87c`,
  `5faca6fc43a7`, `8043295890fd`, `ee495f65b1b5`; `2de80b070bd6`'s replay now stops
  at a different stale frame (450) before its own (3060).
- **Nineteen "clean buffer differs from its file on disk" signatures**, all flaky at
  a 0% replay rate. `0a81fa8b3` makes the oracle save both sides' bytes beside the
  next sighting (`mismatch-*.buffer` / `.disk`, summary inlined in FINDING.md);
  chase them from that evidence. Second pass (same day): the runner threw a known
  signature's later sightings away, so that evidence never reached a bundle --
  `8381257b6` keeps them under `sightings/`. The first bytes in hand fixed two
  causes: adopting the disk baseline left the undo clean point behind
  (`994643047`; code.py, trailing-ws.txt -- which writer made the file equal the
  dirty buffer is still not identified) and Revert File split a binary file's lone
  CRs into lines (`571042f73`; binary.bin, deterministic). The other signatures
  (bom, crlf, link-to-crlf, mixed, tabs, data.json, ...) wait for their sightings.
- **Reopened after this pass's fixes, waiting for a post-fix sighting:**
  `d36a78ef295c` (an Untitled buffer missing after kill -9 although `193dda043`
  arms the flush for every buffer -- seen at `8c740eb00` in two cells). Note that a
  `QA-STALE` signature keys on its shape only, so `49a23ffecdf7`/`2eef96d45d54`
  reopening means "some one-/two-clip stale frame", not the status-bar defect.
- **Banner over another pane's strip** (`95c502fb591f`, `62b9153820d6`): the
  control source lays the banner on `layout.editor_surface`, the whole editor area,
  not the active pane -- check which rect paint uses with a split open.
- Not yet looked at: the undo-algebra findings (5), the leak findings (fds,
  threads, children), the idle-wake and latency findings, the two crashes
  (`707c77a79c1d`, `7ab9a5476905`, flaky), focus-on-a-hidden-overlay
  (`1dd177bb2104`) and the status-bar Ln/Col disagreement (`5a2cb8ba7f6b`).
- **Containment breach, 2026-11-09 06:23 (fixed; one question open).** A soak
  run's git escaped its scratch project into the checkout: "qa commit 29" on
  main, `qa-branch-0..3`, two stash/pop pairs, the checkout switched to another
  branch, and `qa-branch-3` pushed to GitHub. Mechanism: the project sat under
  `build/soak/runs/`, its `.git` was no longer a repository (the monkey's
  `delete-file` creates `.git/qa-deleted`, so a removed `.git` comes back as a
  plain directory), the app's `HasGitMarker` still read it as one, and git's
  upward search found the checkout -- for the monkey's `git -C` and the app's own
  git (which is how the switch and the push happened). Fixed by a ceiling in the
  app's single git choke point (`Git/CommandNeverReachesAnEnclosingRepository`),
  the harness fences (`tools/qa/test_containment.py`) and the runner's alarm
  (`AGENT.md` § A containment breach). Open: what removed the project's `.git` in
  the first place (the app's tree delete, or a monkey op) is not identified.
- **A truncated tab's background one device pixel past its pane** (soak
  `49a23ffecdf7` sighting at `9e17f0048`, release, 1440x900 at ui.scale 2): a
  narrow pane's last tab ("Welc…") painted its background over the divider at
  x=1151 in the full frame; a partial frame clipped to the pane did not. The
  strip layout keeps tabs inside fractional-width strips
  (`TabStripService/EditorStripContentsStayInsideANarrowStrip`), so the extra
  pixel comes from the strip rect or the paint. The sighting's replay is not
  deterministic. Its signature now carries `clip-edge|line` (`8ff6e03f7`), so its
  next sighting is filed on its own.
- **An editable Welcome placeholder, and a breadcrumb that changed undamaged**
  (soak `538a455016ef`, its 640x480 ui.scale 1.25 replay, 936 actions after
  ddmin; the replay's step `type alpha` after `focus-other-group 999999`). The
  top pane's strip showed `*empty.txt` while the pane drew the Welcome text with
  the typed `a` in it ("amicroide"), and the pane's breadcrumb went from
  "Welcome" to "Untitled" with no rect requested for it -- the partial frame kept
  "Welcome". Two defects: (1) a keystroke reached the welcome surface's viewport
  (and its `placeholder` flag cleared) while a file tab was active in that
  group; (2) the breadcrumb label is drawn from state that more than one path
  changes, and no prep-time check repaints it when it changes -- the
  `PreparedFrameState::painted_*` per-group pattern fits (`painted_breadcrumb`,
  damaging `EditorGroupRect::breadcrumb`). Neither reproduced in a short fixture
  (closing a pane's last tab and typing, or with a split and
  `focus-other-group`), so both wait for a shorter replay or a fixture that
  reaches an editable placeholder.
- **The stale-paint signature's `elsewhere` surface is a catch-all.** A box whose
  centre is on no surface the `ui` query names (`ControlUiQuery.cpp` lists nine:
  no splitter, no merge/compare pane, no gap beside a scrollbar) gets
  `elsewhere`, so `ee7ad7b12a7e` (`elsewhere|outside|area`) held three unrelated
  defects after its first fix: the merge panes' shared scroll (`8fc1d580f`), and
  two sightings at `6da237807` not yet chased. Naming more rects in the `ui`
  answer, or keying `elsewhere` on the nearest surface, would split them.
  Most of the merging was a units bug, fixed in `498eea75b`: the line's bbox and
  first pixel are device pixels, the clips and surfaces logical units, so at
  density 2 (most soak cells) nearly every box landed `elsewhere|outside`. The
  line now carries `scale=` and the bucketing scales by it; what is left of this
  entry is the genuinely unnamed rects.
- **`ExternalRepoChange/IdenticalRewriteConfirmsOffTheShellThread` and
  `IdenticalRewriteWithSplitViewsReadsOnce` flaked under load (fixed).** About 1
  in 48 and 1 in 64 with 48 copies in parallel. Instrumented, the failing runs
  posted the second phase's confirm and never applied it before the assertions:
  `FileReadService::FlushPendingReads` (and `CancelAllAndFlush`) returned early on
  "nothing pending, nothing in the mailbox", which is also what a flush sees
  between a worker dropping its pending count and posting its completion. The
  shortcut is gone (`FileReadService/AFlushNeverReturnsBeforeTheCompletionLands`,
  failing 2000 of 2000 before); 0 in 384 and 0 in 192 after. The production
  callers are project switch and teardown, where the late completion arrived
  after the project it was read for.

**Closed 2026-11-25.** Every finding this triage named has since resolved in
the soak's ledger: the deterministic stale frames (9e662fe598fe, 7f858b8312bb,
b5c3ab96f87c, 5faca6fc43a7, 8043295890fd, ee495f65b1b5, 2de80b070bd6), the
truncated tab (49a23ffecdf7), the Welcome placeholder (538a455016ef), the
`elsewhere` catch-all (ee7ad7b12a7e) and the banner overlaps (95c502fb591f,
62b9153820d6) are closed; the lost Untitled buffer (d36a78ef295c), focus on a
hidden overlay (1dd177bb2104) and the Ln/Col disagreement (5a2cb8ba7f6b) are
verified; the crashes are closed or fixed; the "clean buffer differs from its
file" family is 37 verified, 3 fixed, 2 closed and none open; the Escape drag
was fixed today. What is still open lives in the ledger, where it is tracked:
the leak and undo findings (claimed) and the idle wake-up (reopened). One
question stays unanswered and is recorded here rather than tracked: what
removed the project's `.git` before the 2026-11-09 containment breach (the
ceiling, fences and alarm make it harmless now).

### TD-2026-11-11-444 — a cold structural refinement of a heavily churned file costs several times the old intraline pass. [CLOSED 2026-11-25 (decided)]

`microide_diff_bench`, min of runs, interleaved with `main` on the same machine,
four files diffed across ~4 months of history (table in
`openspec/changes/archive/2026-11-11-structural-diff/bench.md`): code files
2-4x the line-era cost (`CompareModel.cpp`, 110 hunks: 1.5 ms -> 5.8 ms), and a
Markdown file rewritten end to end ~9x (1.5 ms -> 13.8 ms). The compare perf
scenarios are at parity, allocations included, and a rebuild of an open compare
refines only the hunks that changed (`HunkRefinementCache`), so this is the cost
of the FIRST build of an extreme diff, not of typing.

What is left, measured: an A* step costs ~100 ns (hash probe, stack-group
reads, heuristic, bucket push for ~3-5 edges), and prose gaps keep the search
because Markdown's backtick spans are string literals on both sides, which the
atom-only shortcut must not skip (a replaced literal is a graph edge). Leads, in
order of expected value: (1) a `Replaced`-aware flat matcher so literal-bearing
atom gaps skip the graph too; (2) packing a vertex's identity into one 64-bit
key and dropping `Vertex::stack` group reads from the hot path; (3) a lower
search cap for gaps that contain no list at all. Gate each on the bench table.

**2026-11-25: the vertex table, not the vertex key.** A profile (perf works on
this machine now) put `Search::Insert` first, and the cost was the table, not
the hashing: every search reset it to 64 slots and doubled it, each doubling a
freshly zeroed allocation (the large ones mmap'd and faulted in). Slots now
carry the search's epoch, so a search neither zeroes nor regrows a table a
previous one sized, and the rehash buffer is kept. Refinement, min of 3x15
interleaved runs, results identical: `CompareModel.cpp` 5.10 -> 3.46 ms,
`PerfMain.cpp` 3.63 -> 2.64, `EditorViewRenderer.cpp` 1.56 -> 1.29, the
Markdown file 13.0 -> 8.6. Still open: leads (2)'s key packing and (3), against
the new numbers.

Lead (1) measured 2026-11-13 and NOT taken: an exact DP aligner for atom-only
gaps with literals on both sides (the search's own edge costs, tabulated; its
cost matched the A* path cost on 600 random gaps) took 206 of the Markdown
file's 111,061 vertices and 34 of `PerfMain.cpp`'s 26,221 -- such gaps are a
rounding error, and the DP always fills n*m cells where A* runs near-linear on
near-identical gaps; wall time showed no gain. The Markdown cost is in gaps
that CONTAIN lists (link `[text](url)` pairs, parentheses in prose), so the
remaining leads are (2) and (3), plus a profile: `perf` needs
`kernel.perf_event_paranoid` <= 2 on this machine (it is 4) and valgrind is not
installed, so the next pass should start by getting one of them.

**Closed 2026-11-25, decided.** After the vertex-table change the first build
costs 2.3x the line era for code (`CompareModel.cpp` 3.46 vs 1.51 ms) and 5.6x
for the Markdown rewrite (8.6 vs 1.54 ms), and what is left is the search over
gaps that CONTAIN lists -- the structural diff itself. Lead (3) cannot move it:
lead (1)'s count put list-free gaps at 206 of 111,061 vertices on that file.
Lead (2) aimed at the vertex key, and the profile now has `Search::Insert` at
under 2% of the bench's samples. It is paid once per diff (a rebuild re-refines
only the changed hunk, `HunkRefinementCache`) and is well inside the 16 ms
frame for every file measured. Reopen with a profile if a real file's first
build costs a frame.

### TD-2026-11-19-453 — a data loss found and two findings read to the bottom of their evidence. [CLOSED 2026-11-25]

Left by the 2026-11-19 bug-hunting pass (commits bbb153be3 onwards). None has a
fix, and each names what it needs; the first is the one that loses a user's work.

1. **Unsaved text in a diff or a merge result lost by a crash: FIXED
   2026-11-20.** A diff's editable side (52e960935, bc0f32527) and a merge's
   typed result (this pass: the record carries the text and its conflict spans;
   a git conflict's merge, which the session does not keep, leaves an editor
   record) are staged and restored, and `documents` lists both so the kill
   oracle holds them to it. Left as the item's history until the entry moves to
   the archive with its other two items.
2. **The mirror engine logs no decision per path: FIXED 2026-11-21** (746a30c2d,
   recorded here 2026-11-25). `TraceMirror` logs each `ReconcileDeleteLocked`,
   `PushNow`, keep/take and retry outcome with the entry's flags under
   `MICROIDE_TRACE_MIRROR_SYNC`, which the QA driver arms, and the `remote:`
   oracle attaches the path's history to the finding.
3. **`RemoteServer/PortsSubscribePushesANewListener` failed once in CI's ubsan
   job** (run 44556917266, d3034cc03: "the spawned process's port is pushed
   with an owner") and never locally. 381bd7bad fixes one way there -- the
   sampler compared port numbers, so a number taken over inside a sample period
   was never attributed -- without showing it is the way CI took; 76b802be0
   makes the failure print the last push. (381bd7bad's message names 63acfda9c
   for that run. It was d3034cc03.) Read the message of the next failure before
   anything else.
4. **Workspace code still waits for git on the shell thread, and no lint says
   so: FIXED 2026-11-25** for every site this item listed. Opening a diff of any
   size, the commit picker's diff, a branch review's file list, reopening an open
   diff, restoring a session's diffs and renaming under one all read git on the
   reader thread (`git/CompareTabLoad`; a restored diff reads nothing until it is
   first shown). The lint is `CheckWorkspaceGitCallsRatchetDown`, a cap of 26
   that ratchets down: what it still counts is the sidebar's stage, unstage and
   discard, the compare picker's history and recent-commit lists, the
   repository probe, and the synchronous fallbacks of a `DiffTabCoordinator`
   built without its async hooks (tests).

**Closed 2026-11-25.** Items 1, 2 and 4 are fixed (above). Item 3 has not
recurred in the six days of CI since 381bd7bad (every `checks` run's ubsan job
included); 76b802be0's message prints the last push, so a recurrence files
itself with its evidence -- open a new entry from that message if it does.

### TD-2026-11-23-454 — the bulk bandwidth estimator reads bunched acks as a faster link. [FIXED 2026-11-25]

`RemoteFrameTransport::HandleAck` measures bandwidth as the bytes between two acks
over the time between them, smoothed 1/8. Acks that arrive bunched -- a congested
return path, a peer or relay that woke late -- come microseconds apart, and a
window's worth of them reads the link at many times its rate: 1.7x to 1160x on a
12.5 MB/s link under load, 35-56 GB/s after a 150 ms ack stall. The window then
jumps to its 1 MiB cap: more bulk in flight than the link holds, queued ahead of
keystrokes. It is the remaining cause of the loaded-box flake of
`RemoteFrameTransport/BulkWindowSettlesAtTheBandwidthDelayProduct` (the relay's
own share was fixed in f74b4266d).

- **Reproduction:** `RemoteFrameTransport/BulkBandwidthIgnoresCompressedAcks`
  (`DelayRelay::SetClientStalled` holds the acks for 150 ms mid-transfer, then
  releases them) fails every run: `build/ec-out/bulk-ab/estimator2-full.patch` on the dev PC.
- **Tried, 2026-11-23**, each A/B'd interleaved under load against the current
  estimator (`build/ec-out/bulk-ab/NOTES.md`, `ab.txt`, `ab2.txt`):
  1. A delivery rate over a whole flight (BBR's sample): a window-limited flow
     then measures window / RTT, and `BulkWindowFillsALongLink` took 24 s, not 4.
  2. The longer of the ack span and the send span, over at least 4 ms: every
     late wake of the sender lowered a sample; median bandwidth 79 % of the link
     against 105 %, throughput 74 % against 90 %.
  3. The send span only when the acks came more than 2x closer than the sends:
     compression gone (0 windows over 137 % against 9 of 60), median bandwidth
     96.7 % against 100.2 %, but one run collapsed to 10 % of the link: with a
     standing queue at the bottleneck a late sender spreads its sends while the
     acks stay at link pace, the rule misfires, and the window falls into
     TD-2026-11-23-455.
- **Why the sender cannot tell:** from its side, bunched acks and a queue that
  smooths late sends look alike. The signal is the receiver's own arrival
  spacing (its receive-side rate), as TCP timestamps and QUIC's ack delay carry.
- **Needs a decision:** that is a wire change -- an Ack's payload is a strict 8
  bytes (`ReadFrames` closes on anything else), so an old peer would drop the
  link. Wants the user's call on the format and on whether client and server
  always upgrade together.

**Fixed 2026-11-25** with the wire change (the user's call: client and server upgrade together; protocol 6, minimum 6). An Ack is 16 bytes: the bytes received and the receiver's steady-clock time of the read that completed them. `HandleAck` samples bandwidth as bytes over the RECEIVER's span between two acks, gathered over at least `kMinBandwidthSpan` (1 ms) and half a chunk, restarted whenever the sender had no bulk queued and whenever a bulk frame leaves an empty pipe, so idle time never enters a sample. `RemoteFrameTransport/BulkBandwidthIgnoresCompressedAcks` failed on the old estimator (peak 71 GB/s on a 12.5 MB/s link) and passes; 0 failures in 112 runs of the transport tests at 16 in parallel. The per-version terminal wire options (v3 colours, v4 edits) went with the old minimum.

### TD-2026-11-23-455 — after a slow stretch, a remote bulk transfer stays at its window floor's rate. [FIXED 2026-11-25]

A link that is slow for a while (a congested stretch) drives the bulk window down
to its floor, 64 KiB, and the transfer then stays at ~1.4 MB/s however fast the
link becomes: 12.5 MB/s in the reproduction, a ninth of it, for good. The floor
equals `RemoteFrameTransport::kMaxBulkChunkBytes`, so only one frame is in flight
there; the bandwidth sample (the gap between two frames' acks) then spans a whole
round trip and reads window / RTT, and the rule `bw x (base RTT + kQueueAllowance)
- one chunk` lands below the floor again. An absorbing state, reached by any dip
of the estimate deep enough, not only by a real slow stretch.

- **Reproduction:** `RemoteFrameTransport/BulkWindowClimbsBackFromItsFloor` (a
  0.5 MB/s stretch, then the relay at 12.5 MB/s; fails 3 of 3, window 65536,
  1.4 MB/s) with `DelayRelay::SetBytesPerSecond`: in
  `build/ec-out/chunk-ab/chunk-32k-and-floor-test.patch` on the dev PC.
- **Tried, 2026-11-23:** bulk frames of 32 KiB (`kMaxBulkChunkBytes` halved, and
  `ProcessTable`'s own 64 KiB cut of process output pointed at it), so two frames
  fit the floor. Sender-side only, no wire change. Under load (40 interleaved
  pairs): the floor test failed 9 of 40 instead of 40 of 40, and
  `BulkWindowSettlesAtTheBandwidthDelayProduct` failed 9 of 40 instead of 1 --
  twice the frames per burst of bunched acks feed TD-2026-11-23-454 twice the
  samples. Parked.
- **Rejected:** raising the floor to 128 KiB: twice the bulk queued ahead of a
  keystroke on a slow link (256 ms instead of 131 ms at 0.5 MB/s).
- **Needs:** TD-2026-11-23-454 first; then two frames at the floor (the 32 KiB
  cut) should hold.

**Fixed 2026-11-25**, in two parts. Bulk frames are 32 KiB (`kMaxBulkChunkBytes`; `ProcessTable` cuts process output at it), so the floor holds two frames and the receiver's clock sees them arrive at the link's pace. And the rule's queue term is `max(5 ms of bandwidth, one chunk)` (`BulkWindowTarget`): with the old `bw x (base RTT + 5 ms) - chunk` the queue went negative below ~6.5 MB/s, so a window-limited sample (window / RTT) always targeted less than the window -- the absorbing state. Now such a sample holds the window and the link-rate samples raise it. `RemoteFrameTransport/BulkWindowClimbsBackFromItsFloor` failed on the old code (1.4 MB/s, window 65536) and, after the first half alone, still 1 run in 48 under load; with both, 0 in 112.

### TD-2026-11-11-440 — the structural diff reads a lexer's tree, not a grammar's. [CLOSED 2026-11-25 (won't do)]

The compare view's structural refinement (`src/compare/DiffSyntax.*`,
`StructuralDiff.*`, openspec `structural-diff`) builds each hunk side's syntax
forest from a language-aware lexer plus bracket matching, with the rules
(`DiffSyntax`) derived from the language contract: comment tokens and bracket
pairs. difftastic parses with tree-sitter, so its lists are real syntax nodes --
an `if` owns its condition and its block, a call owns its callee. Here those are
siblings inside the enclosing brackets, which makes some matches coarser than
difftastic's: a novel `if (x) {` head is three atoms and a list rather than one
node, and the graph search cannot prefer "the same statement, edited" over "a
different statement" beyond what bracket nesting says.

Deliberately not taken now: a grammar per language is a C dependency plus one
generated parser per language (several MB each), and the lexer covers every
language the editor knows. The seam is ready for it: `BuildDiffSyntaxTree` is the
only producer of a `DiffSyntaxTree`, and the search reads nothing but nodes,
tokens and content ids. A grammar-backed builder for the top languages, with the
lexer kept as the fallback, is the follow-up; gate it on `microide_diff_bench`.

**Closed 2026-11-25, won't do** (the user's call): no grammar dependency. The lexer covers every language the editor knows at no per-language binary cost, and the seam stays where this entry put it -- `BuildDiffSyntaxTree` is the only producer of a `DiffSyntaxTree` -- should a grammar-backed builder ever be wanted.

### TD-2026-11-13-449 — SDL's X11 clipboard can paste the previous text right after another application copies. [CLOSED 2026-11-25 (upstream, not reported)]

Found standing up the QA driver's foreign clipboard (`App.foreign_clipboard`,
`tools/qa/x_clipboard.py`): a paste a few milliseconds after another client took
the CLIPBOARD returned the text of the paste BEFORE it, into a buffer confirmed
empty -- about one paste in three in `edit-foreign-clipboard` without a settle.

The mechanism is SDL 3.2's X11 backend (`SDL_x11clipboard.c` / `SDL_x11events.c`):
on the XFixes notice that the owner changed, SDL asks the new owner for TARGETS
(into the `SDL_FORMATS` property). A paste then runs `GetSelectionData`, which
converts into the `SDL_SELECTION` property and waits on `selection_waiting` --
a flag ANY SelectionNotify clears, the TARGETS answer included. It then reads
`SDL_SELECTION`, which still holds the previous paste (read with delete=False),
and returns it. The same read also happens after its 1 s timeout, where it then
takes the selection itself with empty data.

Who meets it: a paste right after a copy elsewhere, and more often with a
clipboard manager, which re-takes the selection after every copy. The app cannot
tell: `SystemClipboard::Read` gets well-formed text. The fix is SDL's: match the
SelectionNotify to the request (its target and property), and delete or clear
the property before converting. Report upstream; the QA scenario settles 0.5 s
before pasting meanwhile, as a person does.

**Closed 2026-11-25** (the user's call): no upstream report from here. Nothing in the app can tell the stale text from a fresh paste, so there is nothing to fix in this tree; the QA scenario keeps its 0.5 s settle. Reopen if a user meets it, with the mechanism above as the report.

### TD-2026-11-16-451 — the rr on the soak machine is patched for tee(2); upstream is not. [CLOSED 2026-11-25 (patch kept local)]

`rr record` aborts the whole recording -- the app with it -- at a system call
it does not know ("Expected ENOSYS for 'tee' ... unsupported by rr",
`src/record_syscall.cc`). Ubuntu's coreutils (`cat`, `head`, `seq`, `yes`)
call `tee` on pipes, so a recorded soak job lost its recorder the first time a
terminal command or a task ran one: twice in 500 steps of the `terminal`
profile, four times in 240 of `world` (2026-11-16, rr 5.10.0 at a808942).

Fixed here, not upstream: `tools/soak/rr-tee.patch` (twelve lines: `tee` writes
no user memory, so it is an `IrregularEmulatedSyscall` that may switch, beside
`splice`) is applied to `build/rr-src` and installed to `/usr/local/bin/rr`.
Rebuilding rr from a fresh checkout loses it: `git apply` the patch first, then
`cmake --build obj && sudo cmake --install obj`, then `chmod a+rX` the
installed files (the install runs under umask 0077).

Still open: send the patch upstream, and until a release carries it, a machine
with a stock rr lists `terminal` and `world` in `soak.py`'s
`UNRECORDED_PROFILES`. A run that loses its recorder to any other system call
reports a `recorder` finding the ledger does not book; `soak.py health` counts
them, and that count is the signal to look for the next one.

**Closed 2026-11-25** (the user's call): the patch is not sent upstream. It stays in `tools/soak/rr-tee.patch` with the rebuild steps above, and a machine with a stock rr lists `terminal` and `world` in `UNRECORDED_PROFILES`; `soak.py health`'s `recorder` count remains the signal for the next unsupported call.

### TD-2026-11-26-457 — the `qa-remote` lane is red: conflict rows, host deletes and renames, host terminals. [RESOLVED 2026-11-26]

`tools/run-checks.sh qa-remote` (not in CI) failed 9-10 checks on the loopback
and slow-link hosts: a settled conflict stayed in the Conflicts view, host-side
deletes and renames never became one-sided rows, Take Remote / Keep Local timed
out, host terminals and the reconnecting row misbehaved. Bisected (last green
5f851c488) to 23689d858, "LifecycleCoordinator reaches persistence writes,
remote hosts and the control channel directly": binding `MakeRemoteHostService()`
into the coordinator's Services made the service when the coordinator was first
built -- before `RegisterLifecycleWakeEvents` had registered the `project-files`
event it set as its wake channel at construction. Its mailbox kept channel 0, so
no reply from the host server woke the loop. Fixed: the channel is given to the
service where every other wake consumer gets it, after registration, whenever the
service was made (`RemoteHostView/TheHostServiceWakesTheLoopWhenEverItWasMade`,
failing first). The loopback suite: 10 failed -> 0.

### TD-2026-11-24-456 — `WorkspaceShell` is still the god class; its wiring factories are the largest functions. [DECIDED 2026-11-26: standing policy]

A 2026-11-24 cleanup pass (duplication, long functions, facades, dead code; net
-2,100 lines) left these on purpose, because each needs a design decision rather
than a mechanical change:

- **The shell's surface.** ~1,000 `WorkspaceShell::` definitions across the
  companion TUs and a 2,200-line `WorkspaceShellMembers.inc`. The declaration
  budget keeps it from growing but also blocks splitting its long members
  (`RenderSettingsOverlay` 318 lines, `HoveredTooltip` 300, `ControlUiDetails` 288,
  `HandleScheduledWakeSources` 237, `ReplaceAllProjectSearchMatches` 225): a named
  phase costs a declaration, so the code keeps lambdas instead. Moving a cluster off the shell needs the cluster's
  reads as an interface first; for ControlUi* that is ~50 callbacks, which is the
  next item's problem in another place.
- **Operations structs of `std::function`.** `MakeActionContext` (604 lines),
  `MakeKeyInputCoordinator` (375), `MakeRemoteHostService` (280) and the other
  `Make*` factories wire hundreds of one-line lambdas back into the shell. The
  hooks are now invoked through `util::InvokeIfSet`, but the shape stays: each
  coordinator depends on a bag of shell callbacks rather than on the services it
  uses. The fix is per coordinator: pass the services (`EditorTabService&`,
  `PatchApplyService&`, ...) and delete the hooks that only forwarded to them.
- **Deliberately kept long:** `ActionAvailability::IsEnabled` (one exhaustive
  switch so `-Wswitch` makes each new action decide), and the spec tables
  (`BuiltinKeybindingSpecs`, `BuiltinSettingSpecs`, `WorkspaceCommandSpecs`,
  `WorkspaceMenuSpecs`, `BuildDefaults`), which are data.
- **2026-11-25 pass.** 56 one-line shell forwarders to services were inlined into
  their callers (Members.inc cap 1512 -> 1428); the action context holds
  `CompareReviewService&` instead of seven forwarding hooks; dead code found by
  linking every binary with `--gc-sections --print-gc-sections` (and functions
  only their own tests called, `PatternCache` among them) is gone; comments were
  removed tree-wide (CLAUDE.md: tool directives only). The cluster moves above
  and the remaining `Make*` hooks, which call shell methods with real bodies
  rather than services, are still open. To re-run the dead-code scan: a clang
  build with `-ffunction-sections` and those linker flags, then the symbols every
  link removed minus those `nm` finds in any binary.
- **2026-11-25 second pass.** A window scan for near-duplicates (identifier-
  preserving, literals normalized, 6-line windows) found ~60 real pairs past
  the exact-clone scan; the substantive ones are folded (SDL surfaces own
  through `render::SurfacePtr`, blame overlays, dirty prompts, folding model,
  file watchers, terminal scroll/mouse, LSP didChange/formatting, compare/merge
  mouse layout). The prescribed coordinator fix landed for the hook bags that
  were mostly forwards: `KeyInputCoordinator` (58 hooks), `PanelMouse`,
  `SidebarMouse`, `ChromeMouse`, `TabMouse` and `Lifecycle` coordinators now
  hold a `Services` struct of references. `DebugPaneMouseCoordinator` keeps its
  debug hooks on purpose: `DebugPaneTests` fake them to observe DAP focus
  without a session. What remains of the factories is hooks into shell
  methods with real bodies, which need those bodies moved to a service first.
  The direction is now enforced: `CheckRedrawHooksAreRequestFrame` (no
  per-surface redraw aliases; twenty were folded) and
  `CheckFactoryHooksDoNotForwardToServices` (one-line service forwards counted
  against a cap that ratchets DOWN, 43 after the lifecycle move). The long
  members split without new declarations by moving their deterministic phases
  to file-local free functions: `ReplaceAllProjectSearchMatches` 186 -> 84
  lines, `HoveredTooltip` 259 -> 172, `ControlUiDetails`/`ControlUiButtons`
  lost their repeated JSON and enum code and the compare section. What is left
  on purpose: the 43 forwards sit in services whose hooks are optional
  (`RemoteHostService` branches on a missing `recents_revision`), in classes
  tests build with `{}` hooks (`TabCoordinator`), in the catalog/persistence/
  terminal-panel ring where a lazy hook is what keeps their factories from
  constructing each other recursively, or bind shell state into their
  arguments; `ControlUiButtons`' merge section and `HandleScheduledWakeSources`
  (a flat list of wake sources) need shell members and stay.
- **2026-11-25, later:** `ControlUiDetails` 200 -> 115 lines (the hover popup,
  the action receipt and each editor pane are file-local JSON builders).
- **2026-11-25, later:** `HoveredTooltip` 172 -> 124 lines (its sidebar mode
  row, panel header, debug toolbar and status bar lookups are file-local).
- **2026-11-25, later:** `RenderSettingsOverlay` 284 -> 27 lines: its frame,
  categories, section header, rows, controls and picker are file-local painters
  over the prepared card (no new declaration).
- **2026-11-25, later:** `ProjectCatalogService` takes a `Services` struct
  (`PromptSurfaceService&`, `MenuCoordinator&`); neither factory builds another,
  so they were not part of the factory ring. Cap 43 -> 41.
- **Near-duplicates left in place:** the Lua wrappers in `PluginRuntimeApiInterop`
  and `PluginHostLuaApi.inc` (their scoping is the longjmp-safety contract; fold
  them only with `tools/audit-lua-longjmp.py` in the loop), and the compare
  surface's `PaintCompareLeftSide` / `PaintCompareRightSide` (the right pane is
  editable and paints carets, diagnostics and blame the left does not).

**Decided 2026-11-26:** closed as standing policy rather than kept as an open
item. The direction is enforced by the architecture rules that ratchet it
(`CheckShellFileSize` on `WorkspaceShellMembers.inc`,
`CheckFactoryHooksDoNotForwardToServices`, `CheckRedrawHooksAreRequestFrame`,
`CheckCoordinatorShellConstructors`): new behaviour lands on a service, a
forwarding hook moves to a `Services` reference when its coordinator is next
touched, and each cap only goes down. Cleanup passes start from those caps
(AGENTS.md § Cleanup Passes) rather than from this entry.

