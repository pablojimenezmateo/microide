# Resolved debt ledger — moved 2026-11-25

Moved from `dev-docs/project/known-tech-debt.md`'s open queue as each entry was
resolved or closed, with its resolution appended. Newest first.

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
