# Resolved debt ledger — moved 2026-11-28

Moved from `dev-docs/project/known-tech-debt.md`'s open queue when the last three
entries were resolved, with their resolutions in place. Newest first.

### TD-2026-11-28-458 — a save writes the whole buffer on the main thread. [RESOLVED 2026-11-28]

`TextViewport::Save` serialized the document and wrote it on the main thread: 65 ms
and 437k allocations for a 50 MB buffer on the release build (perf scenario
`editor_save_large_buffer`), 3.4 s on ASan, where qa-deep's monkey met it (runs
46163735387, 46164693799). Decided with the user: asynchronous. A save is now
`PrepareSave` (a `DocumentSnapshot`, its revision and an undo save point) on the
main thread, `WritePreparedSave` on `project::FileWriteService`'s one writer
thread (so writes land in the order asked), and `ApplyWrittenSave` at the next
drain; starting a 50 MB save costs the main thread 0.04 ms and 21 allocations.
What it keeps: an edit made while the write is in flight leaves the buffer dirty,
and undoing it is clean again (the undo history's save point); a document reloaded
meanwhile is not touched; the app's own write is not an outside change while it is
in flight (`save_in_flight`), and a change made by someone else in that window is
re-routed once it lands; close, quit, close-project and rename wait for the write
as they waited for a formatter (`EditorTabState::save_pending`, settled on landing);
a failed write says why whether or not its tab is still open; `Blocking` saves
(Save As, the dirty prompt, overwrite) flush the writer first and stay synchronous;
teardown, project switch and shutdown flush it; a control-channel command replies
after its writes land. Move to the ledger at the next review.

### TD-2026-11-16-452 — what the soak still cannot say about a finding, or reach. [RESOLVED 2026-11-28]

Left by the 2026-11-16 assessment of the soak (`tools/soak`) and the deep review
that followed it the same day. That review closed the first three items the
assessment listed -- a crash has a stack (`platform/CrashDiagnostics.h`, resolved
by the driver and the signature's key), the soak has source coverage (a clang
coverage build in the rotation, `tools/soak/codecov.py`, `soak.py coverage`), and
a replay says where it left the run's path (`monkey_model.Divergence`; `health`
splits "no replay reproduces" by it). Open, in the order they would pay:

1. **Signatures are not defects: DONE 2026-11-28 as families.** Data and undo
   signatures key on the file name, stale-paint ones on the clip count: 1336
   signatures for a few dozen defects. Signatures stay as they are (a replay
   checks its own); `soak.py` groups them into families -- a stale element is one
   family however the frame caught it, an IME composition one whatever the file
   -- `status` lists the stale-paint families too, a check goes to a family
   nobody has measured before a twin of one that was (`next_check`), and
   `ledger claim|fixed|close SIG --family` books every open signature of SIG's
   family at once.
2. **The fault-injection jobs are not recorded: WON'T DO (decided 2026-11-26).**
   rr cannot follow the remote server (it runs under the loopback sshd, not under
   the app), and recording the sshd's whole session tree would make every remote
   job several times slower for findings the convergence oracle already names
   with their replay; a remote finding is chased with its replay and the server's
   log instead.
3. **No write fails on purpose in the running app: the seam landed
   2026-11-25.** A crash-point rule can now fail a durable write with an errno
   (`fail=ENOSPC|EIO|EACCES|EROFS|EDQUOT|EXDEV|<n>:<site>[@<file>][#<nth>]`,
   armed through the same `crash-points` query) at `durable.writing`,
   `durable.fsyncing` and `durable.renaming` (`util::FaultPoint`). Held to it:
   `LocalWriteFaultMatrix/EveryInjectedWriteFailureLeavesTheOldFile` (every
   site x errno x create/replace), `WorkspaceShell/ADiskFailureDuringASaveKeepsTheEditAndSaysWhy`
   and the QA flow `save-disk-full` (the file keeps its bytes, the buffer its
   edit, a row says why, no staging file, the next save writes it). The `crash`
   profile's monkey now does it at random (`diskfull`: a one-shot ENOSPC at a
   random step of saving the focused dirty file, recorded so a replay fails the
   same save). DONE 2026-11-25 for the session and user-config writers
   too: a state write that fails keeps a sticky row up ("Could not save the
   editor's session state: no space left on the device. Unsaved edits are not
   protected against a crash until it saves.") until a later write of it lands
   (`WorkspaceShell/ASessionThatCannotBeSavedSaysUnsavedWorkIsUnprotected`); the
   idle loop polls only while state writes are in flight. The monkey's
   `diskfull` fails the session flush 30% of the time and requires the row and
   its withdrawal; the driver no longer books the app's `persistence: could not
   write` line for a write it failed on purpose. Item 3 is DONE.
4. **A save killed part-way is not held to old-or-new bytes: FIXED
   2026-11-25.** The app logs `buffer-io: writing` with the sha of the exact
   bytes it is about to write (after the save's own transforms), and the point
   kill records the file's sha just before its save trigger; after the relaunch
   the file must hold one of the two (`DataOracle.kill9_and_check`). The
   monkey's `diskfull` rule now names the resolved file, so a save through a
   symlink is failed where it actually writes.
5. **A real crash loses a coverage build's counters: DECIDED 2026-11-25, kept.**
   The fatal-signal handler will not flush the profile: the writer allocates
   and opens files, so a crash inside `malloc` would hang there and the soak
   would book a stall instead of the crash. Crashes are rare; their runs'
   coverage is the price.
6. **The profile is carried across commits: FIXED 2026-11-25.** `soak.py
   coverage --reset` drops the merged profile; the next coverage job starts it
   afresh. Carrying it stays the default (llvm-cov drops changed functions by
   hash), so reset after deleting much code.
7. **Reach the review listed and did not build: all DONE by 2026-11-26.** A
   scripted language server that answers out of order (safety-plan W8; DONE
   2026-11-26: tools/qa/plugins/qa-chaos-lsp in the `tooling` profile, and a
   debug adapter of the same kind, qa-chaos-dap, with `RequestsSettle` holding
   every request ended -- TD-2026-11-08-435 item 5); IME composition (DONE
   2026-11-25: the `compose` control command sends SDL's text-editing events
   through the real input path, and the monkey's `ime` source composes,
   presses keys in between, then commits, cancels or leaves it in flight --
   in the default and editing profiles, failing a composition that reaches the
   buffer before its commit); a Wayland cell in the soak's matrix
   (DONE 2026-11-25: the `wayland` job type, offered where a weston is); the
   `project-search` overlay mode, which no run has ever shown (DONE 2026-11-25:
   dead -- nothing opened it since project search moved to the sidebar; the
   mode, its text-input surface and their ~160 lines of branches are gone);
   timestamps on the app's log lines on the driver's clock, so a trace line can
   be put beside the replay row that caused it (DONE 2026-11-26: each frame
   line ends `mono=<s>`, CLOCK_MONOTONIC as the driver's rows' `t`, and every
   finding bundle has a `timeline.txt` of its last actions and frames in one
   order).
8. **The stall watchdog's signal can interrupt a system call: AUDITED
   2026-11-25.** Its handler (and every other one) is `SA_RESTART`, so reads and
   writes restart; what never restarts is `poll`/`select`/sleeps. Of the
   twelve `poll`s, two treated `EINTR` as a timeout and now retry against the
   remaining time: `WakePipe::PollReadableOrWake` (the LSP transport's reader)
   and the server's `ProcessTable` stdin write, which any signal (SIGCHLD) could
   fail -- a defect for users too, not only under QA.

9. **Left by the 2026-11-17 review** (which tried the deep review in a private
   soak of its branch -- `MICROIDE_SOAK_REF`, `tools/soak/AGENT.md` § A change to
   the soak itself -- and fixed what that showed: replays that changed a
   finding's signature or misread their own path, checks that could not run,
   an inventory that could not either):
   - **Item 1, for stale paint: DONE 2026-11-28.** The clip list is masked whole
     (`findings._MASKS`), so the number of clips no longer splits a signature;
     the re-keyed findings are checked after every family nobody has measured
     (`next_check`), so the cost below is paid at the back of the queue. Was: 703 open paint
     signatures are 408 distinct (surface, clip relation, shape, element,
     reason) contexts; the clip COUNT alone is the other 295, and one plugins
     run booked three signatures that differ in nothing else. Masking it
     (`findings.normalize`) re-keys every paint finding at once: each loses its
     minimal replay and is checked again (three replays, and ddmin for the 171
     deterministic ones). Worth doing together with a wave of paint fixes, not
     alone.
   - **The leak findings are unexplained: TRACKED in the soak ledger 2026-11-28.**
     The children leak is `2d33d0bc377e` (claimed there by the soak-fix agent);
     a soak finding is the ledger's to carry, not this file's. Three sightings show 64-171 `bash`
     children against 8-11 terminal panes the `terminals` query counted, in
     runs of the monkey that opened a terminal per terminal action; 40 plain
     `term`s keep shells and panes equal, and a replay of the 171-shell run
     shows nothing. The monkey no longer opens them by the hundred, so the
     finding will go quiet with its cause unknown; a finding's `state.json` now
     holds the `terminals` and `projects` answers for the next sighting.
   - **A build stopped the runner's loop: FIXED 2026-11-25.** The build runs on
     its own thread on a copy of the state (`BackgroundBuild`); the loop keeps
     reaping, timing out and starting jobs, and folds in what the build changed
     when it ends (`test_background_build`).
   - **Rows of one step were replayed with a settle between them: FIXED
     2026-11-25.** The recorder marks a row `n` when the next one was sent
     with no settle in between (App.settled_since_record), and the replay sends
     such rows back to back, as it does a burst. A 120-step run: 57 of 226 rows
     marked, and its replay followed the run's path.
   - **The QA driver itself aborted once, in CI (2026-11-26): WATCHED 2026-11-28.**
     Not seen in the fifteen `checks` runs since; a recurrence turns CI red with
     every thread's stack, and is investigated then. `run.py suite
     --jobs 4` died on glibc's `malloc_consolidate(): invalid chunk size` (run
     45772974278; once in the 25 `checks` runs before it). The driver's in-process
     Xlib already calls XInitThreads first; with no core and no stack the cause is
     unknown. `run.py` now enables faulthandler for every thread, so the next one
     names the thread and the call.
   - **A lane's seeded monkey is a gate on a moving walk.** Any change to what
     the monkey can do re-rolls what seed 1 does; the lanes now keep a real-size
     window, and `qa-scale`'s layout seed 2 at 1.25 still meets a stale 1x8 px
     line at ui.scale 1.1 (an app paint defect). 2026-11-26: that seed at 1.25 now met a
     terminal row painted 1.6 px past its pane into the status bar (clip rects
     snap outward to device pixels, so a row that did not fit painted where no
     damage reached); fixed in 699dd0265, rows and caret are laid out only when
     they fit. 2026-11-28: `qa-scale` at 8a3d5bb67 is clean (42/42 scenarios and
     both seeds at 1.25 and 1.75). The walk has changed since the monkey gained
     actions, so this says the lane is green, not that the 1.1 line was the same
     defect; if that line is a defect still, the soak's scale cells will show it.

### TD-2026-11-08-435 — the safety program's first pass: what landed, what it found, and what it left. [RESOLVED 2026-11-28]

The 2026-11-08 implementation of `dev-docs/project/safety-plan.md` (its tracker has
the per-workstream state and commits). It made the monkey ~5x faster (in-process
XTEST input, socket queries, a quiet-log settle), gave it fifteen action sources,
coverage steering, a prefix corpus and data-loss/undo/leak/latency oracles, and put
it on a continuous soak runner with a findings ledger, minimizer and FINDING.md
bundles. The first hunts (12 seeds x 600 steps, then the runner) found and this
session fixed, each with a failing-first test:

- a fold chord in the same event batch as a click reopened itself (frame-prep
  reveal read the click as a caret move into the fold);
- the window had no minimum size, so below a few hundred pixels the chrome was laid
  out off the window (~70 bounds/overlap findings);
- tabs and close buttons lay under the tab strips' overflow chips (editor strip at
  ui.scale 2; the panel strip always, by 4 px);
- an overlay taller than the editor area was painted under the bottom panel, in full
  frames too;
- a symlink to a directory inside the project re-labelled the watched tree, so every
  later outside edit was reported under the link and open buffers went stale (the
  data-loss oracle; minimized 410 -> 50 actions);
- the `surface` query iterated destroyed JSON temporaries (and the range-for lint now
  knows that shape); the canonical query-verb list had drifted (`settings`);
- the pointer-cursor test sized the scrollbar without the sticky band;
- a click on an overflow chip scrolled a tab strip and requested no repaint of it
  (the CI seed; minimized 271 -> 25 actions), and the strip scroll now requests its
  own repaint;
- the tab tooltip hit-tested the focused group's tabs over the whole tab band, the
  wrong tab in a split (and a memo miss every hover frame);
- a picker's title, empty line and key hint ran past a narrow overlay's box;
- the window's minimum size was in window units, so a raised ui.scale shrank the
  layout below the floor again;
- QA flows that raced once the driver lost xdotool's delays (plugin-api, the
  fold chord) and a pre-existing clang narrowing error in a test.
- frame prep's two-step pane sizing clamped the scroll against a band-less view,
  so typing at the end of a document under a sticky band scrolled the caret out
  and repainted the wrong row (CI seed 4; minimized 185 -> 69 actions). The sizing
  is now one helper (`EditorViewRenderer::SizeViewportWithStickyBand`) shared with
  the mouse coordinator, and a pane whose scroll it moves repaints;
- the QA driver never sized the window: the app opens at its own 1440x900 whatever
  the screen, so `monkey --size` and every scaled run drove a 1440x900 window. The
  driver now places and sizes it at startup, and under `MQA_UI_SCALE` window sizes
  are layout units (`device_size`), which is what made the qa-scale lane's
  size-sensitive flows fail at 1.75.
- the presentation scale was pixels / a whole number of units (1.2503 for a
  1024-pixel window at 1.25), at which no clip aligns to device pixels, so a
  partial frame let a device column of the sidebar's text into the editor (qa
  seed 1). Every target now draws at the exact requested scale
  (`app/RenderPresentation.h`), which also closes the residual of
  TD-2026-10-22-376 for windows that are not a multiple of the scale;
- the compact layout's menu-overflow chevron was missing from the shared hover
  probe, so a move off it that another surface consumed left it lit (qa seed 1);
- the external-change banner squeezed into an editor area with no height (panel
  maximized) put its buttons over the window controls (qa-asan seed 4);
- Tab over a selection ending at a line's start reselected whole lines and moved
  the caret up a line per press, and the line it left kept its highlight (qa-asan
  seed 4): indent/outdent keep the selection's ends and direction, and an edit
  that moves the caret's line repaints the focused pane;
- `minimize.py --check-only` compared replays against the literal `auto`.
- a clean tab opened through a symlink kept its old text when the target was saved
  from its own tab (the watcher names the file written; the sweep matched views by
  path). Views record their symlink target and the change is routed to them too
  (qa-asan seed 3, link-to-crlf.txt).
- children inherited an ignored SIGINT/SIGQUIT from an editor started as a
  background job, so Ctrl+C in the terminal and the debugger's Pause of a program
  gdb was still starting did nothing (the coverage lane's DebugService failure; the
  ctest shards passed only because libuv resets signals). Every signal is now reset
  to its default in the child.

What it left, by workstream:

1. **W9 rules 2 and 3: DONE 2026-11-25 as sweeps.** `CheckHitTestsHonourModals` needs the hit-test call graph
   (which entry points move focus, in what order relative to the modal checks), and
   `CheckPointerOverElementsAreHoverTargets` cannot tie `PointerOver(rect)` to the
   hover catch-all by name (16 of the 35 sites pass a variable called `rect`). Both
   want runtime data instead: a QA/unit sweep that, for every element the `ui` query
   names with a hover visual, moves the pointer onto it and requires
   `HoveredInteractiveRect` to name it, and a sweep that clicks every element with a
   modal up and requires focus not to move.
   The hover sweep landed 2026-11-25:
   `QaFlowRegressionUx/EveryClickableElementTheUiNamesIsAHoverTarget` moves the
   pointer onto every enabled button, tab, row, status segment, menu and overlay
   row the `ui` query names -- beside an editor with the git view, with a diff in
   front, with the file finder open -- and requires the shell's hover target (or
   the diff surface's own hover state) to take it. All pass. And
   `QaFlowRegressionUx/AClickAnywhereBehindAModalMovesNoFocus` clicks every
   element of the window outside the unsaved-changes prompt with the prompt up:
   the prompt, the focus and the active tab stay. Item 1 is DONE as runtime
   sweeps (the static rules stay unbuilt, as this item found they cannot tie
   names to behaviour).
2. **W2 frame-prep invariants: DONE 2026-11-28, decided with the user.** The default
   build has no `NDEBUG`, so the `qa` lane and the soak's default job are the
   assert-armed QA the plan asks for. Of the three checks it named: the painted
   caret's owner against the input's is an oracle, not an abort -- `ui` reports
   `textInput` (where typed text goes) beside `focus` (where the keys go), and
   `microide_qa._text_input_findings` holds them to one surface. It found a defect
   on its first monkey runs: a completion list stayed up when a click moved the
   focus to the sidebar, and typed letters went into the buffer behind it
   (`ModalOverlayLostFocus` now dismisses a list over the editor once neither has
   the keys). Hover painting goes through one `PointerOver` that honours the
   occluder, so it has no second owner to disagree with. View models built before
   the frame's last mutation: the state oracles read the chrome against the buffer
   a frame later and report what stays stale. Damage outside the window: measured
   on the CI evidence, 0 of 267 distinct damage rects lay outside it (3 started at
   x=-6, a partly off-screen tab), so a check would have nothing to find.
3. **W5 remainder.** The local fault matrix covers SIGKILL at every crash point of
   `WriteTextFileAtomically`, and since 2026-11-25 errno injection at its write,
   fsync and rename (TD-452 item 3: ENOSPC, EIO, EROFS, EXDEV, ...; a 5 s fsync
   stall is `delay=5000:durable.fsyncing`) with the editor save shown to keep the
   edit and say why. Replace-in-files and merge writes go through the same
   `FileWriteGate` -> `DurableFile` path and so have its points; since
   2026-11-25 `WorkspaceShell/AReplaceAllThatCannotWriteOneFileKeepsItAndSaysWhich`
   fails one file of a replace-all at each site (the others are replaced, it
   keeps its bytes, the search view names it and why). The tree ops (rename,
   move, create, delete) have crash points (`tree-op.*`), and the kill matrix
   `LocalWriteFaultMatrix/EveryCrashPointOfAnOverwritingTreeOpLeavesTheOldOrTheNewTree`
   found that an overwriting rename or create moved the destination aside
   before putting the new one in place: a kill between left no file at the
   destination at all. A file is now replaced in one `rename(2)` after a hard
   link keeps the old one for the batch's undo, a folder by
   `renameat2(RENAME_EXCHANGE)` (a kill after the swap leaves the old folder
   under the source's name, nothing lost); where neither works (no hard
   links, no exchange) the old two-step path remains. The ops stage under the
   durable writes' own `.<name>.microide-staging.<pid>.<seq>` names, so a
   kill's leftover is skipped by the index, git snapshots and mirror sync and
   swept once its process is gone (the old `.microide-staged-N-<name>` names
   were adopted by the mirror as a rename of the deleted file). The mirror
   matrix keeps to the mirror's own durability (it skips `tree-op.*`); a
   remote delete killed between its local apply and the engine's (deferred)
   journal comes back on relaunch, as any locally missing file with no record
   is pulled from the host. The write-path inventory is in
   `editor-essentials.md` ("Write paths and what holds each to old-or-new
   bytes"). Item 3 is DONE; what it shows unheld (a full disk under the session
   and config writers) is TD-2026-11-16-452 item 3. The monkey cannot fill the disk
   either (needs a size-limited filesystem for its fixture).
4. **W8 sequence fuzzers -- the LSP one landed 2026-11-25, and found a
   deadlock.** `WorkspaceLspClient/AnswersEveryRequestOnceWhateverOrderTheServerReplies`
   drives the client against a scripted server answering late, out of order,
   twice, for ids never asked and with diagnostics for stale versions (four
   seeds): every request must be answered exactly once. It deadlocked: the
   transport's one I/O thread wrote the outbound batch with a blocking write,
   so with the server's stdin full it stopped reading the server's stdout, and a
   server that writes its replies synchronously then stopped reading too --
   language features frozen until the 30 s write timeout killed the session.
   `StdioJsonRpcClientTransport` now writes without blocking from a pending
   buffer and waits for readable-or-writable (LSP and DAP both). The DAP one
   landed the same day: `WorkspaceDapClient/AnswersEveryRequestOnceWhateverOrderTheAdapterReplies`
   (an adapter answering late, out of order and twice, for request seqs never
   sent, with 200 KB output events against 200 KB evaluate requests, `stopped`/
   `continued` events and `runInTerminal` reverse requests between): every
   request answered once by its own response, responses and events reach the
   main thread in the adapter's order, every reverse request is answered. On the
   transport before the fix it fails the same way (answers only after the 30 s
   write timeout). The control-command one too:
   `WorkspaceShellControlSettings/EveryControlLineGetsOneReplyInOrder` sends a
   real shell, over three socket connections, batches of random catalogue
   commands with hostile arguments, every query verb with wrong-typed args,
   malformed lines, odd ids and CRLF endings, each batch split at a random byte,
   with frames rendered between: every line gets exactly one JSON reply carrying
   its own id, in order, and the shell's invariants hold (dropping the reply to
   a malformed line fails it). It found nothing. And `ViewportEditSequenceFuzz`
   is a libFuzzer target (2026-11-25): two to four views of one document take
   inserts, newlines, deletes, selection replaces, multi-caret inserts, undo,
   redo, wrap/size/tab-size changes; every view's derived caches must stay true
   after every step, an undo or redo that reports a change must change the text,
   and undoing everything must give back the loaded text and redoing everything
   the newest edit's. Ten minutes (196k runs) found nothing; its set-cover corpus
   (596 inputs) is committed and the `fuzz` lane runs it. Item 4 is DONE. Previously: **W8 sequence fuzzers** are not built: `ControlCommandFuzz`, `LspSessionFuzz` and
   `DapSessionFuzz` (scripted fake servers answering late, out of order and for old
   versions), and the libFuzzer form of `ViewportEditSequenceFuzz`. The stateful
   multi-view unit sweep (`TextViewport/EditsThroughEveryViewKeepEveryViewsCachesTrue`)
   covers the viewport half with a fixed generator.
5. **Monkey reach: DONE 2026-11-26.** Was: not built (`tools/soak/surface-extra.toml` lists each with what
   covers it instead). Reached since: clipboard from another application (the
   `external` source owns the X CLIPBOARD, `x_clipboard.py`), IME composition (the
   `compose` command and the `ime` source, 2026-11-25), a remote-project soak job
   (`remote`), and maximize/fullscreen (2026-11-25: the `window-manager` job runs
   an openbox on its Xvfb, `MQA_WM`, recorded in the replay header so a replay
   gets one too; its window source presses the app's own maximize by name and
   toggles fullscreen), and a language server that answers out of order inside
   the soak (2026-11-26: the `tooling` profile loads tools/qa/plugins/qa-chaos-lsp,
   whose server answers each request on time, late, twice, with an error, with the
   wrong shape, after a stray id, under a string id or with neither result nor
   error first, and sends stale-version diagnostics and server requests; the
   `RequestsSettle` oracle then holds every request ended within the server's latest
   reply plus a margin), and a debug adapter that does the same (2026-11-26:
   tools/qa/plugins/qa-chaos-dap, a Python adapter for a pretend program, so rr
   records its sessions too; the same oracle on the `status` query's new
   `dapRequestsInFlight`; its first run found the debug toolbar on a compare's
   Previous Change, f992ef5ff), and the project shapes the taskflow fixture
   lacks (2026-11-26: the `external` source's `huge-file`, `empty-folder`,
   `submodule` and `long-history` ops -- a 50 MB file it sometimes opens, nested
   empty folders, a submodule added, moved, dirtied, deinitialized and brought
   back, 3000 commits through fast-import), and multi-project switching
   (2026-11-26: the `project` source opens a second scratch copy, a clone beside
   the run's own, switches between the two and closes the second while the
   first stays open; the data oracle already read every project's buffers).
   Nothing in this item is left unreached.
6. **DONE 2026-11-25:** `FrameLatency` reads each frame's prepare-and-paint
   time off the redraw trace (`elapsed=`) on every timed monkey step and files
   a frame over 0.5 s (slow factor applied) once per frame reason, the trace
   before it as evidence; a timing finding, so the soak re-checks it before
   trusting it. Was: **Oracle not built:** input-to-frame latency with the redraw trace attached. (The
   idle-wake oracle and more always-a-finding log markers landed; `util::Log` has no
   levels, so "any error line" stays the marker list.)
7. **Measurement (W1.6/W1.7): DONE 2026-11-28.** The coverage job is in the rotation
   (the `coverage` build, clang source-based, merged per job into one profile;
   `soak.py coverage` writes lines, functions and branches by area at the
   current commit, and `soak.py health` lists the gaps); the monkey steers by
   its own state x action coverage (`coverage.json`); `surface.py` reports
   reached items per category (a keybinding counts as reached when its command
   ran, not when its chord ran in each context). 2026-11-26: the remote server
   was missing from it -- 1.2% of src/server, because its counters never reached
   the profile (sshd dropped LLVM_PROFILE_FILE, the daemon `_exit`ed without
   writing, the host's stop killed it first); fixed in 285bf3ba9 (a remote run:
   2.7% -> 66%). 2026-11-28: a job type with 20 runs in a row that brought no new
   state, transition or finding is on a plateau and keeps a quarter of its weight
   (`note_novelty`, `job_weight`; `health` names it), so the rotation's effort goes
   to the types still finding new ground -- measured first, no type was on one; and
   each coverage report lists the branch conditions on the lines changed since the
   report before it that the soak executed (`codecov.changed_since`).
8. **W10 remainder: DONE 2026-11-26.** Mutation testing over the oracle code:
   `tools/run-checks.sh mutation` (`tools/qa/mutation-test.sh`, mutmut over
   `monkey_oracles.py` against `test_monkey_oracles.py`) fails on any mutant that
   survives or that no test runs. From 990 of 3401 killed (846 surviving, 1546 in
   functions no test called) to all 3199 killed or hanging the tests; nineteen
   lines are marked `# pragma: no mutate`, each a change no outcome can tell
   apart (a timing instant, a falsy default, a codec name's case), and code
   whose mutants were equivalent only because it was redundant was rewritten
   instead (the UTF-8 boundaries are the decoder's own reading, the leak
   evidence is formatters over a thin /proc reader). It found one oracle
   defect: a merge result the kill's trigger had edited was reported lost
   (`d7023aa2f`). `--only <Class.method>` reruns one function's mutants in
   seconds. The C++ redraw verifier is held by its negative controls
   (`tools/check-invariants-bite.sh`), not here.
9. **DONE by 2026-11-25 (the ledger):** the focus-on-a-hidden-surface
   findings are verified (c0c5719bdbce, 1dd177bb2104, b6cc343b6ee1) and the
   stale-paint and latency families of those hunts are closed. Kept for the
   record: **Findings of the first hunts not yet fixed:** several redraw-verifier findings
   (one is a panel tab's close-button hover left painted after a burst with Ctrl+wheel
   zoom), a 4 s answer to `type` of a Hebrew string (editing near the 300 KB
   single-line file under load), and a focus left on a hidden panel (first seen on a
   tiny window; may be gone with the minimum size). They are in the soak ledger
   (`tools/soak/soak.py report`) with their bundles.
11. **Layouts below the 640x400-unit floor: FIXED 2026-11-25.** The window is
    drawn at the requested scale only while it holds the floor; otherwise at the
    largest 0.05 step that does, never below the display's own scale
    (`util::ComputeWindowPresentation`; ui.scale 3 on 1440x900 lays out 640x400
    units at 2.25). The minimum window size still follows the REQUESTED scale,
    so a window that can grow does. `LAYOUT_FLOOR` stays in the QA driver for a
    window smaller than 640x400 pixels at scale 1.
12. **A caret on the last line could stay out of view under a sticky band:
    FIXED 2026-11-25.** Two defects. The sizing loop re-targeted the scroll the
    band-less pass had clamped, not the one asked for; and at the band-less
    bottom of a document a band whose fold ends before the deepest scroll the
    band allows has no consistent scroll at all. The band rule now takes the
    pane's height and keeps an opener there only if its fold still holds the
    top line at that deepest scroll (`ComputeStickyScrollLinesUncached`), so a
    function running to the end keeps its band and a block that ends just
    before does not. `QaFlowRegression/TheCaretOnTheLastLineStaysInViewWhereAFoldEndsNearTheEnd`
    sweeps 87 shapes (failed at a 23-line tail before).
10. **The soak runner's acceptance: DONE 2026-11-28.** A private soak (`MICROIDE_SOAK_REF`)
    on main plus two planted defects -- the status bar's Ln/Col segment dropping its
    text from the fingerprint (stale paint) and a `.md` save dropping its last byte
    after the `writing` trace (lost save) -- ran 24 h unattended from 2026-11-26 08:30
    and again from 2026-11-28 17:11. Both were found within two hours (the stale one
    17,348 sightings, the lost save 38); both minimized to 2 actions; each reads as
    one family (16 stale signatures, 1 lost-save); after the revert both minimal
    replays came back clean (`890be5505c29` 20:05, `1acec4466a3a` 20:49) and the stale
    twins verify by exposure. What the run showed and what was fixed for it:
    checks went strictly by severity, so the paint family was never minimized in the
    first 24 h (`next_check`, 44a7e704a); verifications waited behind every check
    (`next_replay_kind`); a stale frame's family split by the reason and clip
    relation (352518b8d); a fix was booked a signature at a time (`ledger --family`);
    the runner sat idle 5.5 h under the disk floor and logged nothing
    (`idle_note`).
